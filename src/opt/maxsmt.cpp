/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    maxsmt.cpp

Abstract:
   
    MaxSMT optimization context.

Author:

    Nikolaj Bjorner (nbjorner) 2013-11-7

Notes:

--*/

#include <typeinfo>
#include "maxsmt.h"
#include "fu_malik.h"
#include "maxres.h"
#include "maxhs.h"
#include "bcd2.h"
#include "wmax.h"
#include "maxsls.h"
#include "ast_pp.h"
#include "uint_set.h"
#include "opt_context.h"
#include "theory_wmaxsat.h"


namespace opt {

    maxsmt_solver_base::maxsmt_solver_base(
        context& c, vector<rational> const& ws, expr_ref_vector const& soft):
        m_s(c.get_solver()), 
        m(c.get_manager()), 
        m_c(c),
        m_cancel(false), m_soft(m),
        m_assertions(m) {
        c.get_base_model(m_model);
        SASSERT(m_model);
        updt_params(c.params());
        init_soft(ws, soft);
    }
    
    void maxsmt_solver_base::updt_params(params_ref& p) {
        m_params.copy(p);
    }       

    void maxsmt_solver_base::init_soft(vector<rational> const& weights, expr_ref_vector const& soft) {
        m_weights.reset();
        m_soft.reset();
        m_weights.append(weights);
        m_soft.append(soft);
    }

    void maxsmt_solver_base::init() {
        m_lower.reset();
        m_upper.reset();
        m_assignment.reset();
        for (unsigned i = 0; i < m_weights.size(); ++i) {
            expr_ref val(m);
            VERIFY(m_model->eval(m_soft[i].get(), val));                
            m_assignment.push_back(m.is_true(val));
            if (!m_assignment.back()) {
                m_upper += m_weights[i];
            }
        }
        
        TRACE("opt", 
              tout << "upper: " << m_upper << " assignments: ";
              for (unsigned i = 0; i < m_weights.size(); ++i) {
                  tout << (m_assignment[i]?"T":"F");
              }
              tout << "\n";);
    }

    expr* maxsmt_solver_base::mk_not(expr* e) {
        if (m.is_not(e, e)) {
            return e;
        }
        else {
            return m.mk_not(e);
        }
    }

    void maxsmt_solver_base::set_mus(bool f) {
        params_ref p;
        p.set_bool("minimize_core", f);
        m_s.updt_params(p);
    }

    void maxsmt_solver_base::enable_sls(expr_ref_vector const& soft, vector<rational> const& ws) {
        m_c.enable_sls(soft, ws);
    }

    void maxsmt_solver_base::set_enable_sls(bool f) {
        m_c.set_enable_sls(f);
    }

    app* maxsmt_solver_base::mk_fresh_bool(char const* name) {
        app* result = m.mk_fresh_const(name, m.mk_bool_sort());
        m_c.fm().insert(result->get_decl());
        return result;
    }

    smt::theory_wmaxsat* maxsmt_solver_base::get_wmax_theory() const {
        smt::theory_id th_id = m.get_family_id("weighted_maxsat");
        smt::theory* th = m_c.smt_context().get_theory(th_id);               
        if (th) {
            return dynamic_cast<smt::theory_wmaxsat*>(th);
        }
        else {
            return 0;
        }
    }

    smt::theory_wmaxsat* maxsmt_solver_base::ensure_wmax_theory() {
        smt::theory_wmaxsat* wth = get_wmax_theory();
        if (wth) {
            wth->reset_local();
        }
        else {
            wth = alloc(smt::theory_wmaxsat, m, m_c.fm());
            m_c.smt_context().register_plugin(wth);
        }
        return wth;
    }

    maxsmt_solver_base::scoped_ensure_theory::scoped_ensure_theory(maxsmt_solver_base& s) {
        m_wth = s.ensure_wmax_theory();
    }
    maxsmt_solver_base::scoped_ensure_theory::~scoped_ensure_theory() {
        //m_wth->reset_local();
    }
    smt::theory_wmaxsat& maxsmt_solver_base::scoped_ensure_theory::operator()() { return *m_wth; }
    
    void maxsmt_solver_base::trace_bounds(char const * solver) {
        IF_VERBOSE(1, 
                   rational l = m_adjust_value(m_lower);
                   rational u = m_adjust_value(m_upper);
                   if (l > u) std::swap(l, u);
                   verbose_stream() << "(opt." << solver << " [" << l << ":" << u << "])\n";);        
    }



    maxsmt::maxsmt(context& c):
        m_s(c.get_solver()), m(c.get_manager()), m_c(c), m_cancel(false), 
        m_soft_constraints(m), m_answer(m) {}

    lbool maxsmt::operator()(solver* s) {
        lbool is_sat;
        m_msolver = 0;
        symbol const& maxsat_engine = m_c.maxsat_engine();
        IF_VERBOSE(1, verbose_stream() << "(maxsmt)\n";);
        TRACE("opt", tout << "maxsmt\n";);
        if (m_soft_constraints.empty()) {
            TRACE("opt", tout << "no constraints\n";);
            m_msolver = 0;
            is_sat = m_s.check_sat(0, 0);
        }
        else if (maxsat_engine == symbol("maxres")) {            
            m_msolver = mk_maxres(m_c, m_weights, m_soft_constraints);
        }
        else if (maxsat_engine == symbol("mus-mss-maxres")) {            
            m_msolver = mk_mus_mss_maxres(m_c, m_weights, m_soft_constraints);
        }
        else if (maxsat_engine == symbol("mss-maxres")) {            
            m_msolver = mk_mss_maxres(m_c, m_weights, m_soft_constraints);
        }
        else if (maxsat_engine == symbol("bcd2")) {
            m_msolver = mk_bcd2(m_c, m_weights, m_soft_constraints);
        }
        else if (maxsat_engine == symbol("maxhs")) {                
            m_msolver = mk_maxhs(m_c, m_weights, m_soft_constraints);
        }
        else if (maxsat_engine == symbol("sls")) {                
            // NB: this is experimental one-round version of SLS
            m_msolver = mk_sls(m_c, m_weights, m_soft_constraints);
        }        
        else if (is_maxsat_problem(m_weights) && maxsat_engine == symbol("fu_malik")) {
            m_msolver = alloc(fu_malik, m_c, m_soft_constraints);
        }
        else {
            if (maxsat_engine != symbol::null && maxsat_engine != symbol("wmax")) {
                warning_msg("solver %s is not recognized, using default 'wmax'", 
                            maxsat_engine.str().c_str());
            }
            m_msolver = mk_wmax(m_c, m_weights, m_soft_constraints);
        }

        if (m_msolver) {
            m_msolver->updt_params(m_params);
            m_msolver->set_adjust_value(m_adjust_value);
            is_sat = (*m_msolver)();
            if (is_sat != l_false) {
                m_msolver->get_model(m_model);
            }
        }

        // Infrastructure for displaying and storing solution is TBD.
        IF_VERBOSE(1, verbose_stream() << "is-sat: " << is_sat << "\n";
                   if (is_sat == l_true) {
                       verbose_stream() << "Satisfying soft constraints\n";
                       display_answer(verbose_stream());
                   });

        DEBUG_CODE(if (is_sat == l_true) {
                       verify_assignment();
                   });

        // TBD: check that all extensions are unsat too
        
        return is_sat;
    }

    void maxsmt::verify_assignment() {
        // TBD: have to use a different solver 
        // because we don't push local scope any longer.
        return;
        solver::scoped_push _sp(m_s);
        commit_assignment();
        if (l_true != m_s.check_sat(0,0)) {
            IF_VERBOSE(0, verbose_stream() << "could not verify assignment\n";);
            UNREACHABLE();
        }
    }

    bool maxsmt::get_assignment(unsigned idx) const {
        if (m_msolver) {
            return m_msolver->get_assignment(idx);
        }
        else {
            return true;
        }
    } 

    rational maxsmt::get_lower() const {
        rational r = m_lower;
        if (m_msolver) {
            rational q = m_msolver->get_lower();
            if (q > r) r = q;
        }
        return m_adjust_value(r);
    }

    rational maxsmt::get_upper() const {
        rational r = m_upper;
        if (m_msolver) {
            rational q = m_msolver->get_upper();
            if (q < r) r = q;
        }
        return m_adjust_value(r);
    }

    void maxsmt::update_lower(rational const& r) {
        if (m_lower > r) m_lower = r;
    }

    void maxsmt::update_upper(rational const& r) {
        if (m_upper < r) m_upper = r;
    }    

    void maxsmt::get_model(model_ref& mdl) {
        mdl = m_model.get();
    }

    void maxsmt::commit_assignment() {
        for (unsigned i = 0; i < m_soft_constraints.size(); ++i) {
            expr_ref tmp(m);
            tmp = m_soft_constraints[i].get();
            if (!get_assignment(i)) {
                tmp = m.mk_not(tmp);
            }
            TRACE("opt", tout << "committing: " << tmp << "\n";);
            m_s.assert_expr(tmp);            
        }
    }

    void maxsmt::add(expr* f, rational const& w) {
        TRACE("opt", tout << mk_pp(f, m) << " weight: " << w << "\n";);
        SASSERT(m.is_bool(f));
        SASSERT(w.is_pos());
        m_soft_constraints.push_back(f);
        m_weights.push_back(w);
        m_upper += w;
    }

    void maxsmt::display_answer(std::ostream& out) const {
        for (unsigned i = 0; i < m_soft_constraints.size(); ++i) {
            out << mk_pp(m_soft_constraints[i], m)
                << (get_assignment(i)?" |-> true\n":" |-> false\n");
        }
    }
    
    void maxsmt::set_cancel(bool f) {
        m_cancel = f;
        
        if (m_msolver) {
            m_msolver->set_cancel(f);
        }
    }
    
    bool maxsmt::is_maxsat_problem(vector<rational> const& ws) const {
        for (unsigned i = 0; i < ws.size(); ++i) {
            if (!ws[i].is_one()) {
                return false;
            }
        }
        return true;
    }

    void maxsmt::updt_params(params_ref& p) {
        m_params.append(p);
        if (m_msolver) {
            m_msolver->updt_params(p);
        }
    }

    void maxsmt::collect_statistics(statistics& st) const {
        if (m_msolver) {
            m_msolver->collect_statistics(st);
        }
    }


};
