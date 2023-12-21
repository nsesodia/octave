////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 1996-2023 The Octave Project Developers
//
// See the file COPYRIGHT.md in the top-level directory of this
// distribution or <https://octave.org/copyright/>.
//
// This file is part of Octave.
//
// Octave is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Octave is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Octave; see the file COPYING.  If not, see
// <https://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////

#if ! defined (octave_ov_cx_mat_h)
#define octave_ov_cx_mat_h 1

#include "octave-config.h"

#include <cstdlib>

#include <iosfwd>
#include <string>

#include "mx-base.h"
#include "str-vec.h"

#include "error.h"
#include "oct-stream.h"
#include "ov-base.h"
#include "ov-base-mat.h"
#include "ov-typeinfo.h"

#include "MatrixType.h"

class octave_value_list;

// Complex matrix values.

class OCTINTERP_API octave_complex_matrix : public octave_base_matrix<ComplexNDArray>
{
public:

  octave_complex_matrix ()
    : octave_base_matrix<ComplexNDArray> () { }

  octave_complex_matrix (const ComplexNDArray& m)
    : octave_base_matrix<ComplexNDArray> (m) { }

  octave_complex_matrix (const ComplexMatrix& m)
    : octave_base_matrix<ComplexNDArray> (m) { }

  octave_complex_matrix (const ComplexMatrix& m, const MatrixType& t)
    : octave_base_matrix<ComplexNDArray> (m, t) { }

  octave_complex_matrix (const Array<Complex>& m)
    : octave_base_matrix<ComplexNDArray> (ComplexNDArray (m)) { }

  octave_complex_matrix (const ComplexDiagMatrix& d)
    : octave_base_matrix<ComplexNDArray> (ComplexMatrix (d)) { }

  octave_complex_matrix (const ComplexRowVector& v)
    : octave_base_matrix<ComplexNDArray> (ComplexMatrix (v)) { }

  octave_complex_matrix (const ComplexColumnVector& v)
    : octave_base_matrix<ComplexNDArray> (ComplexMatrix (v)) { }

  octave_complex_matrix (const octave_complex_matrix& cm)
    : octave_base_matrix<ComplexNDArray> (cm) { }

  ~octave_complex_matrix () = default;

  octave_base_value * clone () const
  { return new octave_complex_matrix (*this); }
  octave_base_value * empty_clone () const
  { return new octave_complex_matrix (); }

  type_conv_info numeric_demotion_function () const;

  octave_base_value * try_narrowing_conversion ();

  builtin_type_t builtin_type () const { return btyp_complex; }

  bool is_complex_matrix () const { return true; }

  bool iscomplex () const { return true; }

  bool is_double_type () const { return true; }

  bool isfloat () const { return true; }

  double double_value (bool = false) const;

  float float_value (bool = false) const;

  double scalar_value (bool frc_str_conv = false) const
  { return double_value (frc_str_conv); }

  float float_scalar_value (bool frc_str_conv = false) const
  { return float_value (frc_str_conv); }

  NDArray array_value (bool = false) const;

  Matrix matrix_value (bool = false) const;

  FloatMatrix float_matrix_value (bool = false) const;

  Complex complex_value (bool = false) const;

  FloatComplex float_complex_value (bool = false) const;

  ComplexMatrix complex_matrix_value (bool = false) const;

  FloatComplexMatrix float_complex_matrix_value (bool = false) const;

  ComplexNDArray complex_array_value (bool = false) const { return m_matrix; }

  FloatComplexNDArray float_complex_array_value (bool = false) const;

  boolNDArray bool_array_value (bool warn = false) const;

  charNDArray char_array_value (bool frc_str_conv = false) const;

  SparseMatrix sparse_matrix_value (bool = false) const;

  SparseComplexMatrix sparse_complex_matrix_value (bool = false) const;

  octave_value as_double () const;
  octave_value as_single () const;

  octave_value diag (octave_idx_type k = 0) const;

  octave_value diag (octave_idx_type m, octave_idx_type n) const;

  void increment () { m_matrix += Complex (1.0); }

  void decrement () { m_matrix -= Complex (1.0); }

  void changesign () { m_matrix.changesign (); }

  bool save_ascii (std::ostream& os);

  bool load_ascii (std::istream& is);

  bool save_binary (std::ostream& os, bool save_as_floats);

  bool load_binary (std::istream& is, bool swap,
                    octave::mach_info::float_format fmt);

  bool save_hdf5 (octave_hdf5_id loc_id, const char *name, bool save_as_floats);

  bool load_hdf5 (octave_hdf5_id loc_id, const char *name);

  int write (octave::stream& os, int block_size,
             oct_data_conv::data_type output_type, int skip,
             octave::mach_info::float_format flt_fmt) const
  {
    // Yes, for compatibility, we drop the imaginary part here.
    return os.write (matrix_value (true), block_size, output_type,
                     skip, flt_fmt);
  }

  void print_raw (std::ostream& os, bool pr_as_read_syntax = false) const;

  mxArray * as_mxArray (bool interleaved) const;

  octave_value map (unary_mapper_t umap) const;

private:

  DECLARE_OV_TYPEID_FUNCTIONS_AND_DATA
};

#endif
