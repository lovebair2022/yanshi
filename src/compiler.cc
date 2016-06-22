#include "compiler.hh"
#include "fsa_anno.hh"
#include "loader.hh"
#include "option.hh"

#include <algorithm>
#include <ctype.h>
#include <limits.h>
#include <map>
#include <sstream>
#include <stack>
#include <unordered_map>
using namespace std;

map<DefineStmt*, FsaAnno> compiled;

static void print_assoc(const FsaAnno& anno)
{
  magenta(); printf("=== Associated Expr of each state\n"); sgr0();
  REP(i, anno.fsa.n()) {
    printf("%ld:", i);
    for (auto aa: anno.assoc[i]) {
      auto a = aa.first;
      printf(" %s(%ld-%ld", a->name().c_str(), a->loc.start, a->loc.end);
      if (a->entering.size())
        printf(",>%zd", a->entering.size());
      if (a->leaving.size())
        printf(",%%%zd", a->leaving.size());
      if (a->finishing.size())
        printf(",@%zd", a->finishing.size());
      if (a->transiting.size())
        printf(",$%zd", a->transiting.size());
      printf(")");
    }
    puts("");
  }
  puts("");
}

static void print_automaton(const Fsa& fsa)
{
  magenta(); printf("=== Automaton\n"); sgr0();
  green(); printf("start: %ld\n", fsa.start);
  red(); printf("finals:");
  for (long i: fsa.finals)
    printf(" %ld", i);
  puts("");
  sgr0(); puts("edges:");
  REP(i, fsa.n()) {
    printf("%ld:", i);
    for (auto it = fsa.adj[i].begin(); it != fsa.adj[i].end(); ) {
      long from = it->first.first, to = it->first.second, v = it->second;
      while (++it != fsa.adj[i].end() && to == it->first.first && it->second == v)
        to = it->first.second;
      if (from == to-1)
        printf(" (%ld,%ld)", from, v);
      else
        printf(" (%ld-%ld,%ld)", from, to-1, v);
    }
    puts("");
  }
  puts("");
}

Expr* find_lca(Expr* u, Expr* v)
{
  if (u->depth > v->depth)
    swap(u, v);
  if (u->depth < v->depth)
    for (long k = 63-__builtin_clzl(v->depth-u->depth); k >= 0; k--)
      if (u->depth <= v->depth-(1L<<k))
        v = v->anc[k];
  if (u == v)
    return u;
  if (v->depth)
    for (long k = 63-__builtin_clzl(v->depth); k >= 0; k--)
      if (k < u->anc.size() && u->anc[k] != v->anc[k])
        u = u->anc[k], v = v->anc[k];
  return u->anc[0]; // NULL if two trees
}

struct Compiler : Visitor<Expr> {
  stack<FsaAnno> st;
  stack<Expr*> path;
  long tick = 0;

  void pre_expr(Expr& expr) {
    expr.pre = tick++;
    expr.depth = path.size();
    if (path.size()) {
      expr.anc.assign(1, path.top());
      for (long k = 1; 1L << k <= expr.depth; k++)
        expr.anc.push_back(expr.anc[k-1]->anc[k-1]);
    } else
      expr.anc.assign(1, nullptr);
    path.push(&expr);
    DP(5, "%s(%ld-%ld)", expr.name().c_str(), expr.loc.start, expr.loc.end);
  }
  void post_expr(Expr& expr) {
    path.pop();
    expr.post = tick;
#ifdef DEBUG
    st.top().fsa.check();
#endif
  }

  void visit(Expr& expr) override {
    pre_expr(expr);
    expr.accept(*this);
    post_expr(expr);
  }
  void visit(BracketExpr& expr) override {
    st.push(FsaAnno::bracket(expr));
  }
  void visit(CollapseExpr& expr) override {
    st.push(FsaAnno::collapse(expr));
  }
  void visit(ComplementExpr& expr) override {
    visit(*expr.inner);
    st.top().complement(&expr);
  }
  void visit(ConcatExpr& expr) override {
    visit(*expr.rhs);
    FsaAnno rhs = move(st.top());
    visit(*expr.lhs);
    st.top().concat(rhs, &expr);
  }
  void visit(DifferenceExpr& expr) override {
    visit(*expr.rhs);
    FsaAnno rhs = move(st.top());
    visit(*expr.lhs);
    st.top().difference(rhs, &expr);
  }
  void visit(DotExpr& expr) override {
    st.push(FsaAnno::dot(&expr));
  }
  void visit(EmbedExpr& expr) override {
    st.push(FsaAnno::embed(expr));
  }
  void visit(EpsilonExpr& expr) override {
    st.push(FsaAnno::epsilon_fsa(&expr));
  }
  void visit(IntersectExpr& expr) override {
    visit(*expr.rhs);
    FsaAnno rhs = move(st.top());
    visit(*expr.lhs);
    st.top().intersect(rhs, &expr);
  }
  void visit(LiteralExpr& expr) override {
    st.push(FsaAnno::literal(expr));
  }
  void visit(PlusExpr& expr) override {
    visit(*expr.inner);
    st.top().plus(&expr);
  }
  void visit(QuestionExpr& expr) override {
    visit(*expr.inner);
    st.top().question(&expr);
  }
  void visit(RepeatExpr& expr) override {
    visit(*expr.inner);
    st.top().repeat(expr);
  }
  void visit(StarExpr& expr) override {
    visit(*expr.inner);
    st.top().star(&expr);
  }
  void visit(UnionExpr& expr) override {
    visit(*expr.rhs);
    FsaAnno rhs = move(st.top());
    visit(*expr.lhs);
    st.top().union_(rhs, &expr);
  }
};

void compile(DefineStmt* stmt)
{
  if (compiled.count(stmt))
    return;
  FsaAnno& anno = compiled[stmt];
  Compiler comp;
  comp.visit(*stmt->rhs);
  anno = move(comp.st.top());
  anno.determinize();
  anno.minimize();
  DP(4, "size(%s::%s) = %ld", stmt->module->filename.c_str(), stmt->lhs.c_str(), anno.fsa.n());
}

void compile_actions(DefineStmt* stmt)
{
  FsaAnno& anno = compiled[stmt];
  auto find_within = [&](long u) {
    vector<pair<Expr*, ExprTag>> within;
    Expr* last = NULL;
    sort(ALL(anno.assoc[u]), [](const pair<Expr*, ExprTag>& x, const pair<Expr*, ExprTag>& y) {
      if (x.first->pre != y.first->pre)
        return x.first->pre < y.first->pre;
      return x.second < y.second;
    });
    for (auto aa: anno.assoc[u]) {
      Expr* stop = last ? find_lca(last, aa.first) : NULL;
      last = aa.first;
      for (Expr* x = aa.first; x != stop; x = x->anc[0])
        within.emplace_back(x, aa.second);
    }
    sort(ALL(within));
    return within;
  };
  decltype(anno.assoc) withins(anno.fsa.n());
  REP(i, anno.fsa.n())
    withins[i] = move(find_within(i));

  auto get_code = [](Action* action) {
    if (auto t = dynamic_cast<InlineAction*>(action))
      return t->code;
    else if (auto t = dynamic_cast<RefAction*>(action))
      return t->define_module->defined_action[t->ident];
    return string();
  };

#define D(S) \
            if (auto t = dynamic_cast<InlineAction*>(action)) \
              printf(S " %ld %ld %ld %s\n", u, e.first, v, t->code.c_str()); \
            else if (auto t = dynamic_cast<RefAction*>(action)) \
              printf(S " %ld %ld %ld %s\n", u, e.first, v, t->define_module->defined_action[t->ident].c_str());
#undef D
#define D(S)

  if (output_header)
    fprintf(output_header, "long yanshi_%s_transit(long u, long c);\n", stmt->lhs.c_str());
  fprintf(output, "long yanshi_%s_transit(long u, long c)\n", stmt->lhs.c_str());
  fprintf(output, "{\n");
  indent(output, 1);
  fprintf(output, "long v = -1;\n");
  indent(output, 1);
  fprintf(output, "switch (u) {\n");
  REP(u, anno.fsa.n()) {
    if (anno.fsa.adj[u].empty())
      continue;
    indent(output, 1);
    fprintf(output, "case %ld:\n", u);
    indent(output, 2);
    fprintf(output, "switch (c) {\n");

    unordered_map<long, pair<vector<pair<long, long>>, stringstream>> v2case;
    for (auto it = anno.fsa.adj[u].begin(); it != anno.fsa.adj[u].end(); ) {
      long from = it->first.first, to = it->first.second, v = it->second;
      while (++it != anno.fsa.adj[u].end() && to == it->first.first && it->second == v)
        to = it->first.second;
      v2case[v].first.emplace_back(from, to);
      stringstream& body = v2case[v].second;

      auto ie = withins[u].end(), je = withins[v].end();

      // leaving = Expr(u) - Expr(v)
      for (auto i = withins[u].begin(), j = withins[v].begin(); i != ie; ++i) {
        while (j != je && i->first > j->first)
          ++j;
        if (j == je || i->first != j->first)
          for (auto action: i->first->leaving) {
            D("%%");
            body << "{" << get_code(action) << "}\n";
          }
      }

      // entering = Expr(v) - Expr(u)
      for (auto i = withins[u].begin(), j = withins[v].begin(); j != je; ++j) {
        while (i != ie && i->first < j->first)
          ++i;
        if (i == ie || i->first != j->first)
          for (auto action: j->first->entering) {
            D(">");
            body << "{" << get_code(action) << "}\n";
          }
      }

      // transiting = intersect(Expr(u), Expr(v))
      for (auto i = withins[u].begin(), j = withins[v].begin(); j != je; ++j) {
        while (i != ie && i->first < j->first)
          ++i;
        if (i != ie && i->first == j->first)
          for (auto action: j->first->transiting) {
            D("$");
            body << "{" << get_code(action) << "}\n";
          }
      }

      // finishing = intersect(Expr(u), Expr(v)) & Expr(v).has_final(v)
      for (auto i = withins[u].begin(), j = withins[v].begin(); j != je; ++j) {
        while (i != ie && i->first < j->first)
          ++i;
        if (i != ie && i->first == j->first && long(j->second) & long(ExprTag::final))
          for (auto action: j->first->finishing) {
            D("@");
            body << "{" << get_code(action) << "}\n";
          }
      }
    }

    for (auto& x: v2case) {
      for (auto& y: x.second.first) {
        indent(output, 2);
        if (y.first == y.second-1)
          fprintf(output, "case %ld:\n", y.first);
        else
          fprintf(output, "case %ld ... %ld:\n", y.first, y.second-1);
      }
      indent(output, 3);
      fprintf(output, "v = %ld;\n%s", x.first, x.second.second.str().c_str());
      indent(output, 3);
      fprintf(output, "break;\n");
    }

    indent(output, 2);
    fprintf(output, "}\n");
    indent(output, 2);
    fprintf(output, "break;\n");
  }
  indent(output, 1);
  fprintf(output, "}\n");
  indent(output, 1);
  fprintf(output, "return v;\n");
  fprintf(output, "}\n");
}

void compile_export(DefineStmt* stmt)
{
  DP(2, "Exporting %s", stmt->lhs.c_str());
  FsaAnno& anno = compiled[stmt];

  DP(3, "Construct automaton with all referenced CollapseExpr's DefineStmt");
  vector<vector<Edge>> adj;
  decltype(anno.assoc) assoc;
  vector<vector<DefineStmt*>> cllps;
  long allo = 0;
  unordered_map<DefineStmt*, long> stmt2offset;
  function<void(DefineStmt*)> allocate_collapse = [&](DefineStmt* stmt) {
    if (stmt2offset.count(stmt))
      return;
    DP(4, "Allocate %ld to %s", allo, stmt->lhs.c_str());
    FsaAnno& anno = compiled[stmt];
    long old = stmt2offset[stmt] = allo;
    allo += anno.fsa.n()+1;
    adj.insert(adj.end(), ALL(anno.fsa.adj));
    REP(i, anno.fsa.n())
      for (auto& e: adj[old+i])
        e.second += old;
    adj.emplace_back(); // appeared in other automata, this is a vertex corresponding to the completion of 'stmt'
    assoc.insert(assoc.end(), ALL(anno.assoc));
    assoc.emplace_back();
    FOR(i, old, old+anno.fsa.n())
      if (anno.fsa.has_special(i-old)) {
        for (auto aa: assoc[i])
          if (auto e = dynamic_cast<CollapseExpr*>(aa.first)) {
            DefineStmt* v = e->define_stmt;
            allocate_collapse(v);
            // (i@{CollapseExpr,...}, special, _) -> ({CollapseExpr,...}, epsilon, CollapseExpr.define_stmt.start)
            sorted_emplace(adj[i], epsilon, stmt2offset[v]+compiled[v].fsa.start);
          }
        long j = adj[i].size();
        while (j && AB < adj[i][j-1].first.second) {
          long v = adj[i][j-1].second;
          if (adj[i][j-1].first.first < AB)
            adj[i][j-1].first.second = AB;
          else
            j--;
          for (auto aa: assoc[v])
            if (auto e = dynamic_cast<CollapseExpr*>(aa.first)) {
              DefineStmt* w = e->define_stmt;
              allocate_collapse(w);
              // (_, special, v@{CollapseExpr,...}) -> (CollapseExpr.define_stmt.final, epsilon, v)
              for (long f: compiled[w].fsa.finals) {
                long g = stmt2offset[w]+f;
                sorted_emplace(adj[g], epsilon, v);
                if (g == i)
                  j++;
              }
            }
        }
        // remove (i, special, _)
        adj[i].resize(j);
      }
  };
  allocate_collapse(stmt);
  anno.fsa.adj = move(adj);
  anno.assoc = move(assoc);
  anno.deterministic = false;
  DP(3, "# of states: %ld", anno.fsa.n());

  // substring grammar & this nonterminal is not marked as intact
  if (opt_substring_grammar && ! stmt->intact) {
    DP(3, "Constructing substring grammar");
    anno.substring_grammar();
    DP(3, "# of states: %ld", anno.fsa.n());
  }

  DP(3, "Determinize");
  anno.determinize();
  DP(3, "# of states: %ld", anno.fsa.n());
  DP(3, "Minimize");
  anno.minimize();
  DP(3, "# of states: %ld", anno.fsa.n());
  DP(3, "Keep accessible states");
  anno.accessible();
  DP(3, "# of states: %ld", anno.fsa.n());
  DP(3, "Keep co-accessible states");
  anno.co_accessible();
  DP(3, "# of states: %ld", anno.fsa.n());

  if (opt_dump_automaton)
    print_automaton(anno.fsa);
  if (opt_dump_assoc)
    print_assoc(anno);
}

//// Graphviz dot renderer

void generate_graphviz(Module* mo)
{
  fprintf(output, "// Generated by 偃师, %s\n", mo->filename.c_str());
  for (Stmt* x = mo->toplevel; x; x = x->next)
    if (auto stmt = dynamic_cast<DefineStmt*>(x)) {
      if (stmt->export_) {
        compile_export(stmt);
        FsaAnno& anno = compiled[stmt];

        fprintf(output, "digraph \"%s\" {\n", mo->filename.c_str());
        bool start_is_final = false;

        // finals
        indent(output, 1);
        fprintf(output, "node[shape=doublecircle,color=olivedrab1,style=filled,fontname=Monospace];");
        for (long f: anno.fsa.finals)
          if (f == anno.fsa.start)
            start_is_final = true;
          else
            fprintf(output, " %ld", f);
        fprintf(output, "\n");

        // start
        indent(output, 1);
        if (start_is_final)
          fprintf(output, "node[shape=doublecircle,color=orchid];");
        else
          fprintf(output, "node[shape=circle,color=orchid];");
        fprintf(output, " %ld\n", anno.fsa.start);

        // other states
        indent(output, 1);
        fprintf(output, "node[shape=circle,color=black,style=\"\"]\n");

        // edges
        REP(u, anno.fsa.n()) {
          unordered_map<long, stringstream> labels;
          bool first = true;
          auto it = anno.fsa.adj[u].begin();
          for (; it != anno.fsa.adj[u].end(); ++it) {
            stringstream& lb = labels[it->second];
            if (! lb.str().empty())
              lb << ',';
            if (it->first.first == it->first.second-1)
              lb << it->first.first;
            else
              lb << it->first.first << '-' << it->first.second-1;
          }
          for (auto& lb: labels) {
            indent(output, 1);
            fprintf(output, "%ld -> %ld[label=\"%s\"]\n", u, lb.first, lb.second.str().c_str());
          }
        }
      }
    }
  fprintf(output, "}\n");
}

//// C++ renderer

void generate_cxx_export(DefineStmt* stmt)
{
  compile_export(stmt);
  FsaAnno& anno = compiled[stmt];

  if (output_header)
    fprintf(output_header, "void yanshi_%s_init(long& start, vector<long>& finals);\n", stmt->lhs.c_str());
  fprintf(output, "void yanshi_%s_init(long& start, vector<long>& finals)\n", stmt->lhs.c_str());
  fprintf(output, "{\n");
  indent(output, 1);
  fprintf(output, "start = %ld;\n", anno.fsa.start);
  indent(output, 1);
  fprintf(output, "finals = {");
  bool first = true;
  for (long f: anno.fsa.finals) {
    if (first) first = false;
    else fprintf(output, ",");
    fprintf(output, "%ld", f);
  }
  fprintf(output, "};\n");
  fprintf(output, "}\n\n");

  DP(3, "Compiling actions");
  compile_actions(stmt);
}

void generate_cxx(Module* mo)
{
  fprintf(output, "// Generated by 偃师, %s\n", mo->filename.c_str());
  fprintf(output, "#include <vector>\n");
  fprintf(output, "using std::vector;\n");
  if (opt_standalone) {
    fputs(
"#include <algorithm>\n"
"#include <cstdio>\n"
"using namespace std;\n"
, output);
  }
  if (output_header) {
    fputs(
"#pragma once\n"
"#include <vector>\n"
"using std::vector;\n"
, output_header);
  }
  fprintf(output, "\n");
  for (Stmt* x = mo->toplevel; x; x = x->next)
    if (auto xx = dynamic_cast<DefineStmt*>(x)) {
      if (xx->export_)
        generate_cxx_export(xx);
    } else if (auto xx = dynamic_cast<CppStmt*>(x))
      fprintf(output, "%s", xx->code.c_str());
  if (opt_standalone) {
    fputs(
"\n"
"int main(int argc, char* argv[])\n"
"{\n"
"  long u, len = 0;\n"
"  vector<long> finals;\n"
"  yanshi_main_init(u, finals);\n"
"  if (argc > 1)\n"
"    for (char* c = argv[1]; *c; c++) {\n"
"      u = yanshi_main_transit(u, *(unsigned char*)c);\n"
"      if (u < 0) break;\n"
"      len++;\n"
"    }\n"
"  else {\n"
"    int c;\n"
"    while (u >= 0 && (c = getchar()) != EOF) {\n"
"      u = yanshi_main_transit(u, c);\n"
"      if (u < 0) break;\n"
"      len++;\n"
"    }\n"
"  }\n"
"  printf(\"len: %ld\\nstate: %ld\\nfinal: %s\\n\", len, u, binary_search(finals.begin(), finals.end(), u) ? \"true\" : \"false\");\n"
"}\n"
, output);
  }
}
