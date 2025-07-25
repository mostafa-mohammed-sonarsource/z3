/*++
Copyright (c) 2023 Microsoft Corporation

Module Name:

    euf_arith_plugin.cpp

Abstract:

    plugin structure for arithmetic

Author:

    Nikolaj Bjorner (nbjorner) 2023-11-11

--*/

#include "ast/euf/euf_arith_plugin.h"
#include "ast/euf/euf_egraph.h"
#include <algorithm>

namespace euf {

    arith_plugin::arith_plugin(egraph& g) :
        plugin(g),
        a(g.get_manager()),
        m_add(g, "add", get_id(), OP_ADD),
        m_mul(g, "mul", get_id(), OP_MUL) {
        std::function<void(void)> uadd = [&]() { m_undo.push_back(undo_t::undo_add); };
        m_add.set_undo(uadd);
        std::function<void(void)> umul = [&]() { m_undo.push_back(undo_t::undo_mul); };
        m_mul.set_undo(umul);
        m_add.set_injective();
        auto e = a.mk_int(0);
        auto n = g.find(e) ? g.find(e) : g.mk(e, 0, 0, nullptr);
        m_add.add_unit(n);
        m_mul.add_zero(n);

        e = a.mk_real(0);
        n = g.find(e) ? g.find(e) : g.mk(e, 0, 0, nullptr);
        m_add.add_unit(n);
        m_mul.add_zero(n);

        e = a.mk_int(1);
        n = g.find(e) ? g.find(e) : g.mk(e, 0, 0, nullptr);
        m_mul.add_unit(n);

        e = a.mk_real(1);
        n = g.find(e) ? g.find(e) : g.mk(e, 0, 0, nullptr);
        m_mul.add_unit(n);


    }    

    void arith_plugin::register_node(enode* n) {
        TRACE(plugin, tout << g.bpp(n) << "\n");
        m_add.register_node(n);
        m_mul.register_node(n);
    }

    void arith_plugin::merge_eh(enode* n1, enode* n2) {
        TRACE(plugin, tout << g.bpp(n1) << " == " << g.bpp(n2) << "\n");
        m_add.merge_eh(n1, n2);
        m_mul.merge_eh(n1, n2);
    }

    void arith_plugin::propagate() {
        m_add.propagate();
        m_mul.propagate();
    }

    void arith_plugin::undo() {
        auto k = m_undo.back();
        m_undo.pop_back();
        switch (k) {
        case undo_t::undo_add:
            m_add.undo();
            break;
        case undo_t::undo_mul:
            m_mul.undo();
            break;
        default:
            UNREACHABLE();
        }
    }
        
    std::ostream& arith_plugin::display(std::ostream& out) const {
        m_add.display(out);
        m_mul.display(out);
        return out;
    }
}
