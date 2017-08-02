/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "derivative.hpp"

using namespace std;

namespace casadi {

  Function CentralDiff::create(const std::string& name, int n, const Dict& opts) {
    return Function::create(new CentralDiff(name, n), opts);
  }

  CentralDiff::CentralDiff(const std::string& name, int n)
    : FunctionInternal(name), n_(n) {
  }

  CentralDiff::~CentralDiff() {
  }

  Options CentralDiff::options_
  = {{&FunctionInternal::options_},
     {{"stepsize",
       {OT_DOUBLE,
        "Perturbation size [default: 1e-8]"}},
      {"second_order_stepsize",
       {OT_DOUBLE,
        "Second order perturbation size [default: 1e-3]"}},
      {"scheme",
       {OT_STRING,
        "Differencing scheme [default: 'central']"}}
     }
  };

  void CentralDiff::init(const Dict& opts) {
    // Call the initialization method of the base class
    FunctionInternal::init(opts);

    // Default options
    h_ = 1e-8;
    h2_ = 1e-3;

    // Read options
    for (auto&& op : opts) {
      if (op.first=="stepsize") {
        h_ = op.second;
      } else if (op.first=="second_order_stepsize") {
        h2_ = op.second;
      } else if (op.first=="scheme") {
        casadi_warning("Option 'scheme' currently ignored");
      }
    }

    // Allocate work vector for (perturbed) inputs and outputs
    alloc_w((n_calls()+2) * f().nnz_in(), true);
    alloc_w((n_calls()+2) * f().nnz_out(), true);

    // Allocate sufficient temporary memory for function evaluation
    alloc(f());
  }

  Sparsity CentralDiff::get_sparsity_in(int i) {
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out();
    if (i<n_in) {
      // Non-differentiated input
      return derivative_of_.sparsity_in(i);
    } else if (i<n_in+n_out) {
      // Non-differentiated output
      return derivative_of_.sparsity_out(i-n_in);
    } else {
      // Seeds
      return repmat(derivative_of_.sparsity_in(i-n_in-n_out), 1, n_);
    }
  }

  Sparsity CentralDiff::get_sparsity_out(int i) {
    return repmat(derivative_of_.sparsity_out(i), 1, n_);
  }

  double CentralDiff::default_in(int ind) const {
    if (ind<derivative_of_.n_in()) {
      return derivative_of_.default_in(ind);
    } else {
      return 0;
    }
  }

  size_t CentralDiff::get_n_in() {
    return derivative_of_.n_in() + derivative_of_.n_out() + derivative_of_.n_in();
  }

  size_t CentralDiff::get_n_out() {
    return derivative_of_.n_out();
  }

  std::string CentralDiff::get_name_in(int i) {
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out();
    if (i<n_in) {
      return derivative_of_.name_in(i);
    } else if (i<n_in+n_out) {
      return "out_" + derivative_of_.name_out(i-n_in);
    } else {
      return "fwd_" + derivative_of_.name_in(i-n_in-n_out);
    }
  }

  std::string CentralDiff::get_name_out(int i) {
    return "fwd_" + derivative_of_.name_out(i);
  }

  Function CentralDiff::get_forward(int nfwd, const std::string& name,
                                   const std::vector<std::string>& inames,
                                   const std::vector<std::string>& onames,
                                   const Dict& opts) const {
    Dict opts_mod = opts;
    opts_mod["stepsize"] = h2_;
    return Function::create(new CentralDiff(name, nfwd), opts_mod);
  }

  void CentralDiff::eval(void* mem, const double** arg, double** res, int* iw, double* w) const {
    // Shorthands
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out(), n_calls = this->n_calls();
    int n_x = derivative_of_.nnz_in(), n_f = derivative_of_.nnz_out();

    // Non-differentiated input
    const double* f_arg = w;
    for (int j=0; j<n_in; ++j) {
      const int nnz = derivative_of_.nnz_in(j);
      casadi_copy(*arg++, nnz, w);
      w += nnz;
    }

    // Non-differentiated output
    const double* f_res = w;
    for (int j=0; j<n_out; ++j) {
      const int nnz = derivative_of_.nnz_out(j);
      casadi_copy(*arg++, nnz, w);
      w += nnz;
    }

    // Forward seeds
    const double** seed = arg; arg += n_in;

    // Forward sensitivities
    double** sens = res; res += n_out;

    // Temporary vector for seeds and sensitivities
    double* v = w; w += derivative_of_.nnz_in();
    casadi_fill(v, n_x, 0.);
    double* Jv = w; w += derivative_of_.nnz_out();

    // Work vectors for perturbed inputs and outputs
    double* f_arg_pert = w; w += n_calls * f().nnz_in();
    double* f_res_pert = w; w += n_calls * f().nnz_out();

    // For each derivative direction
    for (int i=0; i<n_; ++i) {
      // Copy seeds to v
      double* v1 = v;
      for (int j=0; j<n_in; ++j) {
        int nnz = derivative_of_.nnz_in(j);
        if (seed[j]) casadi_copy(seed[j] + nnz*i, nnz, v1);
        v1 += nnz;
      }

      // Perturb function argument (depends on differentiation algorithm)
      perturb(h_, n_x, f_arg, f_arg_pert, v);

      // Function evaluation
      double* f_arg_pert1 = f_arg_pert;
      double* f_res_pert1 = f_res_pert;
      for (int c=0; c<n_calls; ++c) {
        // Function inputs
        for (int j=0; j<n_in; ++j) {
          arg[j] = f_arg_pert1;
          f_arg_pert1 += f().nnz_in(j);
        }
        // Function outputs
        for (int j=0; j<n_out; ++j) {
          res[j] = f_res_pert1;
          f_res_pert1 += f().nnz_out(j);
        }
        // Call function
        f()(arg, res, iw, w, 0);
      }

      // Calculate finite difference approximation
      finalize(n_f, f_res, f_res_pert, Jv);

      // Gather sensitivities
      double* Jv1 = Jv;
      for (int j=0; j<n_out; ++j) {
        int nnz = derivative_of_.nnz_out(j);
        if (sens[j]) casadi_copy(Jv1, nnz, sens[j] + i*nnz);
        Jv1 += nnz;
      }
    }
  }

  void CentralDiff::perturb(double h, int n_x, const double* x, double* x_pert, const double* v) {
    casadi_copy(x, n_x, x_pert);
    casadi_axpy(n_x, h/2, v, x_pert);
    casadi_copy(x, n_x, x_pert + n_x);
    casadi_axpy(n_x, -h/2, v, x_pert + n_x);
  }

  void CentralDiff::finalize(int n_f, const double* f, const double* f_pert, double* Jv) const {
    casadi_copy(f_pert, n_f, Jv);
    casadi_axpy(n_f, -1., f_pert + n_f, Jv);
    casadi_scal(n_f, 1/h_, Jv);
  }

} // namespace casadi
