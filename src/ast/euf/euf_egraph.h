/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    euf_egraph.h

Abstract:

    E-graph layer

Author:

    Nikolaj Bjorner (nbjorner) 2020-08-23

Notes:
   
    It relies on
    - data structures form the (legacy) SMT solver.
      - it still uses eager path compression.

    NB. The worklist is in reality inherited from the legacy SMT solver. 
    It is claimed to have the same effect as delayed congruence table reconstruction from egg.
    Similar to the legacy solver, parents are partially deduplicated.
    
--*/

#pragma once
#include "util/statistics.h"
#include "util/trail.h"
#include "util/lbool.h"
#include "util/scoped_ptr_vector.h"
#include "ast/euf/euf_enode.h"
#include "ast/euf/euf_etable.h"
#include "ast/euf/euf_plugin.h"
#include "ast/ast_ll_pp.h"
#include <vector>

namespace euf {

    /***
      \brief store derived theory equalities and disequalities
      Theory 'id' is notified with the equality/disequality of theory variables v1, v2.
      For equalities, v1 and v2 are merged into the common root of child and root (their roots may
      have been updated since the equality was derived, but the explanation for
      v1 == v2 is provided by explaining the equality child == root.
      For disequalities, m_child refers to an equality atom of the form e1 == e2.
      It is equal to false under the current context.
      The explanation for the disequality v1 != v2 is derived from explaining the 
      equality between the expression for v1 and e1, and the expression for v2 and e2 
      and the equality of m_eq and false: the literal corresponding to m_eq is false in the
      current assignment stack, or m_child is congruent to false in the egraph.
    */
    class th_eq {

        theory_id  m_id;
        theory_var m_v1;
        theory_var m_v2;
        union {            
            enode* m_child;
            enode* m_eq;
        };
        enode* m_root;
    public:
        bool is_eq() const { return m_root != nullptr; }
        theory_id id() const { return m_id; }
        theory_var v1() const { return m_v1; }
        theory_var v2() const { return m_v2; }
        enode* child() const { SASSERT(is_eq()); return m_child; }
        enode* root() const { SASSERT(is_eq()); return m_root; }
        enode* eq() const { SASSERT(!is_eq()); return m_eq; }
        th_eq(theory_id id, theory_var v1, theory_var v2, enode* c, enode* r) :
            m_id(id), m_v1(v1), m_v2(v2), m_child(c), m_root(r) {}
        th_eq(theory_id id, theory_var v1, theory_var v2, enode* eq) :
            m_id(id), m_v1(v1), m_v2(v2), m_eq(eq), m_root(nullptr) {}
    };

    // cc_justification contains the uses of congruence closure 
    // It is the only information collected from justifications in order to
    // reconstruct EUF proofs. Transitivity, Symmetry of equality are not
    // tracked.
    typedef std::tuple<app*,app*,uint64_t, bool> cc_justification_record;
    typedef svector<cc_justification_record> cc_justification;
    
    class egraph {        

        friend class plugin;

        typedef ptr_vector<trail> trail_stack;

        enum to_merge_t { to_merge_plain, to_merge_comm, to_justified, to_add_literal };
        struct to_merge {
            enode* a, * b;
            to_merge_t t;
            justification j;
            bool commutativity() const { return t == to_merge_comm; }
            to_merge(enode* a, enode* b, bool c) : a(a), b(b), t(c ? to_merge_comm : to_merge_plain) {}
            to_merge(enode* a, enode* b, justification j): a(a), b(b), t(to_justified), j(j) {}
            to_merge(enode* p, enode* ante): a(p), b(ante), t(to_add_literal) {}
        };

        struct stats {
            unsigned m_num_merge;
            unsigned m_num_th_eqs;
            unsigned m_num_th_diseqs;
            unsigned m_num_lits;
            unsigned m_num_eqs;
            unsigned m_num_conflicts;
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };
        struct update_record {
            struct toggle_cgc {};
            struct toggle_merge_tf {};
            struct add_th_var {};
            struct replace_th_var {};
            struct new_th_eq {};
            struct new_th_eq_qhead {};
            struct plugin_qhead {};
            struct inconsistent {};
            struct value_assignment {};
            struct lbl_hash {};
            struct lbl_set {};
            struct update_children {};
            struct set_relevant {};
            struct plugin_undo {};
            enum class tag_t { is_set_parent, is_add_node, is_toggle_cgc, is_toggle_merge_tf, is_update_children,
                    is_add_th_var, is_replace_th_var, is_new_th_eq,
                    is_lbl_hash, is_new_th_eq_qhead, is_plugin_qhead,
                    is_inconsistent, is_value_assignment, is_lbl_set, is_set_relevant,
                    is_plugin_undo };
            tag_t  tag;
            enode* r1;
            enode* n1;
            union {
                unsigned r2_num_parents;
                struct {
                    unsigned   m_th_id : 8;
                    unsigned   m_old_th_var : 24;
                };
                unsigned qhead;
                bool     m_inconsistent;
                signed char m_lbl_hash;
                unsigned long long m_lbls;
            };
            update_record(enode* r1, enode* n1, unsigned r2_num_parents) :
                tag(tag_t::is_set_parent), r1(r1), n1(n1), r2_num_parents(r2_num_parents) {}
            update_record(enode* n) :
                tag(tag_t::is_add_node), r1(n), n1(nullptr), r2_num_parents(UINT_MAX) {}
            update_record(enode* n, toggle_cgc) :
                tag(tag_t::is_toggle_cgc), r1(n), n1(nullptr), r2_num_parents(UINT_MAX) {}
            update_record(enode* n, toggle_merge_tf) :
                tag(tag_t::is_toggle_merge_tf), r1(n), n1(nullptr), r2_num_parents(UINT_MAX) {}
            update_record(enode* n, unsigned id, add_th_var) :
                tag(tag_t::is_add_th_var), r1(n), n1(nullptr), r2_num_parents(id) {}
            update_record(enode* n, theory_id id, theory_var v, replace_th_var) :
                tag(tag_t::is_replace_th_var), r1(n), n1(nullptr), m_th_id(id), m_old_th_var(v) {}
            update_record(new_th_eq) :
                tag(tag_t::is_new_th_eq), r1(nullptr), n1(nullptr), r2_num_parents(0) {}
            update_record(unsigned qh, new_th_eq_qhead):
                tag(tag_t::is_new_th_eq_qhead), r1(nullptr), n1(nullptr), qhead(qh) {}
            update_record(unsigned qh, plugin_qhead) :
                tag(tag_t::is_plugin_qhead), r1(nullptr), n1(nullptr), qhead(qh) {}
            update_record(bool inc, inconsistent) :
                tag(tag_t::is_inconsistent), r1(nullptr), n1(nullptr), m_inconsistent(inc) {}
            update_record(enode* n, value_assignment) :
                tag(tag_t::is_value_assignment), r1(n), n1(nullptr), qhead(0) {}
            update_record(enode* n, lbl_hash):
                tag(tag_t::is_lbl_hash), r1(n), n1(nullptr), m_lbl_hash(n->m_lbl_hash) {}
            update_record(enode* n, lbl_set):
                tag(tag_t::is_lbl_set), r1(n), n1(nullptr), m_lbls(n->m_lbls.get()) {}    
            update_record(enode* n, update_children) :
                tag(tag_t::is_update_children), r1(n), n1(nullptr), r2_num_parents(UINT_MAX) {}
            update_record(enode* n, set_relevant) :
                tag(tag_t::is_set_relevant), r1(n), n1(nullptr), r2_num_parents(UINT_MAX) {}
            update_record(unsigned th_id, plugin_undo) :
                tag(tag_t::is_plugin_undo), r1(nullptr), n1(nullptr), m_th_id(th_id) {}
        };
        ast_manager&           m;
        svector<to_merge>      m_to_merge;
        etable                 m_table;
        region                 m_region;
        scoped_ptr_vector<plugin> m_plugins;
        svector<update_record> m_updates;
        unsigned_vector        m_scopes;
        enode_vector           m_expr2enode;
        enode*                 m_tmp_eq = nullptr;
        enode*                 m_tmp_node = nullptr;
        unsigned               m_tmp_node_capacity = 0;
        tmp_app                m_tmp_app;
        enode_vector           m_nodes;
        expr_ref_vector        m_exprs;
        func_decl_ref_vector   m_eq_decls;
        vector<enode_vector>   m_decl2enodes;
        enode_vector           m_empty_enodes;
        unsigned               m_num_scopes = 0;
        bool                   m_inconsistent = false;
        enode                  *m_n1 = nullptr;
        enode                  *m_n2 = nullptr;
        justification          m_justification;
        unsigned               m_new_th_eqs_qhead = 0;
        unsigned               m_plugin_qhead = 0;
        svector<th_eq>         m_new_th_eqs;
        bool_vector            m_th_propagates_diseqs;
        enode_vector           m_todo;
        stats                  m_stats;
        bool                   m_uses_congruence = false;
        bool                   m_default_relevant = true;
        uint64_t               m_congruence_timestamp = 0;

        std::vector<std::function<void(enode*,enode*)>> m_on_merge;
        std::function<void(enode*, enode*)>    m_on_propagate_literal;
        std::function<void(enode*)>            m_on_make;
        std::function<void(expr*,expr*,expr*)> m_used_eq;
        std::function<void(app*,app*)>         m_used_cc;  
        std::function<void(std::ostream&, void*)>   m_display_justification;

        void push_eq(enode* r1, enode* n1, unsigned r2_num_parents) {
            m_updates.push_back(update_record(r1, n1, r2_num_parents));
        }
        void push_node(enode* n) { m_updates.push_back(update_record(n)); }

        // plugin related methods
        void push_plugin_undo(unsigned th_id) { m_updates.push_back(update_record(th_id, update_record::plugin_undo())); }
        void push_merge(enode* a, enode* b, justification j) { SASSERT(a->get_sort() == b->get_sort());  m_to_merge.push_back({ a, b, j }); }
        void push_merge(enode* a, enode* b, bool comm) { m_to_merge.push_back({ a, b, comm }); }
        void propagate_plugins();

        void add_th_eq(theory_id id, theory_var v1, theory_var v2, enode* c, enode* r);
        
        void add_th_diseqs(theory_id id, theory_var v1, enode* r);
        bool th_propagates_diseqs(theory_id id) const;
        void add_literal(enode* n, enode* ante);
        void queue_literal(enode* n, enode* ante);
        void undo_eq(enode* r1, enode* n1, unsigned r2_num_parents);
        void undo_add_th_var(enode* n, theory_id id);
        enode* mk_enode(expr* f, unsigned generation, unsigned num_args, enode * const* args);
        void force_push();
        void set_conflict(enode* n1, enode* n2, justification j);
        void merge(enode* n1, enode* n2, justification j);
        void merge_th_eq(enode* n, enode* root);
        void merge_justification(enode* n1, enode* n2, justification j);
        void reinsert_parents(enode* r1, enode* r2);
        void remove_parents(enode* r);
        void unmerge_justification(enode* n1);
        void reinsert_equality(enode* p);
        void update_children(enode* n);
        void push_lca(enode* a, enode* b);
        enode* find_lca(enode* a, enode* b);
        void push_to_lca(enode* a, enode* lca);
        void push_congruence(enode* n1, enode* n2, bool commutative);
        void push_todo(enode* n);
        void toggle_cgc_enabled(enode* n, bool backtracking);

        enode_bool_pair insert_table(enode* p);
        void erase_from_table(enode* p);

        template <typename T>
        void explain_eq(ptr_vector<T>& justifications, cc_justification* cc, enode* a, enode* b, justification const& j);

        template <typename T>
        void explain_todo(ptr_vector<T>& justifications, cc_justification* cc);

        std::ostream& display(std::ostream& out, unsigned max_args, enode* n) const;
        
    public:
        egraph(ast_manager& m);
        ~egraph();

        void add_plugin(plugin* p);
        plugin* get_plugin(family_id fid) const { return m_plugins.get(fid, nullptr); }

        enode* find(expr* f) const { return m_expr2enode.get(f->get_id(), nullptr); }
        enode* find(expr* f, unsigned n, enode* const* args);
        enode* mk(expr* f, unsigned generation, unsigned n, enode *const* args);
        enode_vector const& enodes_of(func_decl* f);
        void push() { if (can_propagate()) propagate(); ++m_num_scopes; }
        void pop(unsigned num_scopes);

        /**
           \brief merge nodes, all effects are deferred to the propagation step.
         */
        void merge(enode* n1, enode* n2, void* reason) { merge(n1, n2, justification::external(reason)); }        
        void new_diseq(enode* n);
        void new_diseq(enode* n, void* reason);


        /**
           \brief propagate set of merges.           
           This call may detect an inconsistency. Then inconsistent() is true.
           Use then explain() to extract an explanation for the conflict.

           It may also infer new implied equalities, when the roots of the 
           equated nodes are merged. Use then new_eqs() to extract the vector
           of new equalities.
        */
        bool propagate();
        bool can_propagate() const { return !m_to_merge.empty(); }
        bool inconsistent() const { return m_inconsistent; }

        /**
        * \brief check if two nodes are known to be disequal.
        */
        bool are_diseq(enode* a, enode* b);

        enode* get_enode_eq_to(func_decl* f, unsigned num_args, enode* const* args);

        enode* tmp_eq(enode* a, enode* b);

        /**
           \brief Maintain and update cursor into propagated consequences.
           The result of get_literal() is a pair (n, is_eq)
           where \c n is an enode and \c is_eq indicates whether the enode
           is an equality consequence. 
         */
        void       add_th_diseq(theory_id id, theory_var v1, theory_var v2, enode* eq);
        bool       has_th_eq() const { return m_new_th_eqs_qhead < m_new_th_eqs.size(); }
        th_eq      get_th_eq() const { return m_new_th_eqs[m_new_th_eqs_qhead]; }
        void       next_th_eq() { force_push(); SASSERT(m_new_th_eqs_qhead < m_new_th_eqs.size()); m_new_th_eqs_qhead++; }

        void set_lbl_hash(enode* n);


        void add_th_var(enode* n, theory_var v, theory_id id);
        void register_shared(enode* n, theory_id id);
        void set_th_propagates_diseqs(theory_id id);
        void set_cgc_enabled(enode* n, bool enable_cgc);
        void set_merge_tf_enabled(enode* n, bool enable_merge_tf);
        
        void set_value(enode* n, lbool value, justification j);
        void set_bool_var(enode* n, unsigned v) { n->set_bool_var(v); }
        void set_relevant(enode* n);
        void set_default_relevant(bool b) { m_default_relevant = b; }

        void set_on_merge(std::function<void(enode* root,enode* other)>& on_merge) { m_on_merge.push_back(on_merge); }
        void set_on_propagate(std::function<void(enode* lit,enode* ante)>& on_propagate) { m_on_propagate_literal = on_propagate; }
        void set_on_make(std::function<void(enode* n)>& on_make) { m_on_make = on_make; }
        void set_used_eq(std::function<void(expr*,expr*,expr*)>& used_eq) { m_used_eq = used_eq; }
        void set_used_cc(std::function<void(app*,app*)>& used_cc) { m_used_cc = used_cc; }
        void set_display_justification(std::function<void (std::ostream&, void*)> & d) { m_display_justification = d; }
        
        void begin_explain();
        void end_explain();
        bool uses_congruence() const { return m_uses_congruence; }
        template <typename T>
        void explain(ptr_vector<T>& justifications, cc_justification* cc);
        template <typename T>
        void explain_eq(ptr_vector<T>& justifications, cc_justification* cc, enode* a, enode* b);
        template <typename T>
        unsigned explain_diseq(ptr_vector<T>& justifications, cc_justification* cc, enode* a, enode* b);
        enode_vector const& nodes() const { return m_nodes; }

        ast_manager& get_manager() { return m; }

        void invariant();
        void copy_from(egraph const& src, std::function<void*(void*)>& copy_justification);
        struct e_pp {
            egraph const& g;
            enode*  n;
            e_pp(egraph const& g, enode* n) : g(g), n(n) {}
            std::ostream& display(std::ostream& out) const { return g.display(out, 0, n); }
        };
        e_pp pp(enode* n) const { return e_pp(*this, n); }
        struct b_pp {
            egraph const& g;
            enode* n;
            b_pp(egraph const& g, enode* n) : g(g), n(n) {}
            std::ostream& display(std::ostream& out) const { return n ? (out << n->get_expr_id() << ": " << mk_bounded_pp(n->get_expr(), g.m)) : out << "null"; }
        };
        b_pp bpp(enode* n) const { return b_pp(*this, n); }
        std::ostream& display(std::ostream& out) const; 
        
        void collect_statistics(statistics& st) const;

        unsigned num_scopes() const { return m_scopes.size() + m_num_scopes; }
        unsigned num_nodes() const { return m_nodes.size(); }
    };

    inline std::ostream& operator<<(std::ostream& out, egraph const& g) { return g.display(out); }
    inline std::ostream& operator<<(std::ostream& out, egraph::e_pp const& p) { return p.display(out); }
    inline std::ostream& operator<<(std::ostream& out, egraph::b_pp const& p) { return p.display(out); }
}
