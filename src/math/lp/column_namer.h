#pragma once
/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    <name>

Abstract:

    <abstract>

Author:

    Lev Nachmanson (levnach)

Revision History:


--*/
#include <string>
#include "math/lp/static_matrix.h"
namespace lp {
class column_namer {
public:
    virtual ~column_namer() = default;
    virtual std::string get_variable_name(unsigned j) const = 0;
    template <typename T>
    std::ostream & print_row(const row_strip<T> & row, std::ostream & out) const {
        vector<std::pair<T, unsigned>> coeff;
        for (auto & p : row) {
            coeff.push_back(std::make_pair(p.coeff(), p.var()));
        }
        print_linear_combination_of_column_indices(coeff, out);
        return out;
    }
    
    template <typename T>
    void print_linear_combination_of_column_indices(const vector<std::pair<T, unsigned>> & coeffs, std::ostream & out) const {
        bool first = true;
        for (const auto & it : coeffs) {
            auto val = it.first;
            if (first) {
                first = false;
            } else {
                if (numeric_traits<T>::is_pos(val)) {
                    out << " + ";
                } else {
                    out << " - ";
                    val = -val;
                }
            }
            if (val == -numeric_traits<T>::one())
                out << " - ";
            else if (val != numeric_traits<T>::one())
                out << val << "*";
        
            out << get_variable_name(it.second);
        }
    }

};
}
