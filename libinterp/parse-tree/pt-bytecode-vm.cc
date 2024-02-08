////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2023-2024 The Octave Project Developers
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

#if defined (HAVE_CONFIG_H)
#  include "config.h"
#endif

#if defined (OCTAVE_ENABLE_BYTECODE_EVALUATOR)

#include <iostream>
#include <iomanip>

#include "time-wrappers.h"

#include "pt-bytecode-vm.h"
#include "pt-bytecode-vm-internal.h"
#include "pt-bytecode-walk.h"
#include "ov.h"
#include "error.h"
#include "symtab.h"
#include "interpreter-private.h"
#include "interpreter.h"
#include "pt-eval.h"
#include "pt-tm-const.h"
#include "pt-stmt.h"
#include "ov-classdef.h"
#include "ov-cs-list.h"
#include "ov-ref.h"
#include "ov-range.h"
#include "ov-inline.h"
#include "ov-cell.h"

#include "ov-vm.h"
#include "ov-fcn-handle.h"
#include "ov-cs-list.h"

//#pragma GCC optimize("O0")

// Returns the uint16 value stored at 'p' taking endianess into account
#ifdef WORDS_BIGENDIAN
#define USHORT_FROM_UCHAR_PTR(p) (((p)[0] << 8) + (p)[1])
#else
#define USHORT_FROM_UCHAR_PTR(p) ((p)[0] + ((p)[1] << 8))
#endif

// Returns the uint16 value from two unsigned chars taking endianess into account
#ifdef WORDS_BIGENDIAN
#define USHORT_FROM_UCHARS(c1,c2) ((c1 << 8) | (c2))
#else
#define USHORT_FROM_UCHARS(c1,c2) ((c1) | (c2 << 8))
#endif

static bool ov_need_stepwise_subsrefs (octave_value &ov);
static void copy_many_args_to_caller (octave::stack_element *sp, octave::stack_element *caller_stack_end,
                                      int n_args_to_move, int n_args_caller_expects);
static int lhs_assign_numel (octave_value &ov, const std::string& type, const std::list<octave_value_list>& idx);

#define TODO(msg) error("Not done yet %d: " msg, __LINE__)
#define ERR(msg) error("VM error %d: " msg, __LINE__)
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond))                                                               \
      ERR("Internal VM conistency check failed, " #cond);                      \
  } while ((0))
#define PANIC(msg) error("VM panic %d: " msg, __LINE__)

using namespace octave;

static unsigned
chars_to_uint (unsigned char *p)
{
  unsigned u = 0;
#ifdef WORDS_BIGENDIAN
  u |= *p++ << 24;
  u |= *p++ << 16;
  u |= *p++ << 8;
  u |= *p;
#else
  u |= *p++;
  u |= *p++ << 8;
  u |= *p++ << 16;
  u |= *p << 24;
#endif

  return u;
}

std::vector<std::pair<int, std::string>>
octave::opcodes_to_strings (bytecode &bc)
{
  return opcodes_to_strings (bc.m_code, bc.m_ids);
}

std::vector<std::pair<int, std::string>>
octave::opcodes_to_strings (std::vector<unsigned char> &v_code, std::vector<std::string> &names)
{
  unsigned char *p = v_code.data ();
  unsigned char *code = p;
  int n = v_code.size ();
  bool wide_opext_active = false;

  // Skip some framedata
  p += 4;

  std::vector<std::pair<int, std::string>> v_pair_row_str;

#define CASE_START(type) \
  case INSTR::type:                     \
  { /* Line stored in s */              \
  std::string s;                        \
  /* Code offset */                     \
  int ip = static_cast<int> (p - code); \
  s += #type;                           \
  /* vec for id names */                \
  std::vector<std::string> v_ids;       \

#define CASE_END()        \
    if (v_ids.size ())                  \
      {                                 \
        s += " #";                      \
        for (auto ss : v_ids)           \
          s += " " + ss;                \
      }                                 \
    v_pair_row_str.push_back ({ip, s}); \
    break;}                             \

#define PRINT_OP(type)    \
  CASE_START (type)       \
  CASE_END ()             \

#define PCHAR() \
    {\
      if (wide_opext_active)                  \
        {                                     \
          wide_opext_active = false;          \
          PSHORT()                            \
        }                                     \
      else                                    \
        {                                     \
          p++;                                \
          CHECK_END ();                       \
          s += " " + std::to_string (*p);     \
        }\
    }

#define PCHAR_AS_CHAR() \
    {p++;                                     \
    CHECK_END ();                             \
    s += std::string {" '"} + static_cast<char> (*p) + "'";}

#define PSHORT() \
    {p++;                                        \
    CHECK_END ();                                \
    unsigned char b0 = *p;                       \
    p++;                                         \
    CHECK_END ();                                \
    unsigned char b1 = *p;                       \
    unsigned u = USHORT_FROM_UCHARS (b0, b1);    \
    s += " " + std::to_string (u);}

#define PSSLOT() \
    {p++;                                                            \
    CHECK_END ();                                                    \
    s += " " + std::to_string (*p);                                  \
    v_ids.push_back (std::string {*p < names.size() ?                \
                                      names[*p].c_str() :            \
                                      "INVALID SLOT"});}

#define PSLOT() \
    {if (wide_opext_active)                                         \
      PWSLOT ()                                                     \
    else                                                            \
      PSSLOT ()                                                     \
    wide_opext_active = false;}

#define PWSLOT() \
    {p++;                                                           \
    CHECK_END ();                                                   \
    unsigned char b0 = *p;                                          \
    p++;                                                            \
    CHECK_END ();                                                   \
    unsigned char b1 = *p;                                          \
    unsigned u = USHORT_FROM_UCHARS (b0, b1);                       \
    s += " " + std::to_string (u);                                  \
    v_ids.push_back (std::string {u < names.size() ?                \
                                      names[u].c_str() :            \
                                      "INVALID SLOT"});}

#define CHECK_END() \
  do {if (p >= v_code.data () + v_code.size ()) { error ("Invalid bytecode\n");}} while((0))

#ifdef WORDS_BIGENDIAN
#define PINT() \
  do {\
    unsigned u = 0;\
    p++;\
    CHECK_END ();\
    u |= *p++ << 24;\
    CHECK_END ();\
    u |= *p++ << 16;\
    CHECK_END ();\
    u |= *p++ << 8;\
    CHECK_END ();\
    u |= *p;\
    s += " " + std::to_string (u);\
  } while (0);
#else
#define PINT() \
  do {\
    unsigned u = 0;\
    p++;\
    CHECK_END ();\
    u |= *p++;\
    CHECK_END ();\
    u |= *p++ << 8;\
    CHECK_END ();\
    u |= *p++ << 16;\
    CHECK_END ();\
    u |= *p << 24;\
    s += " " + std::to_string (u);\
  } while (0);
#endif

  while (p < code + n)
    {
      switch (static_cast<INSTR> (*p))
        {
          PRINT_OP (ANON_MAYBE_SET_IGNORE_OUTPUTS)
          PRINT_OP (EXT_NARGOUT)
          PRINT_OP (POP)
          PRINT_OP (DUP)
          PRINT_OP (MUL)
          PRINT_OP (MUL_DBL)
          PRINT_OP (ADD)
          PRINT_OP (ADD_DBL)
          PRINT_OP (SUB)
          PRINT_OP (SUB_DBL)
          PRINT_OP (DIV)
          PRINT_OP (DIV_DBL)
          PRINT_OP (RET)
          PRINT_OP (RET_ANON)
          PRINT_OP (LE)
          PRINT_OP (LE_DBL)
          PRINT_OP (LE_EQ)
          PRINT_OP (LE_EQ_DBL)
          PRINT_OP (GR)
          PRINT_OP (GR_DBL)
          PRINT_OP (GR_EQ)
          PRINT_OP (GR_EQ_DBL)
          PRINT_OP (EQ)
          PRINT_OP (EQ_DBL)
          PRINT_OP (NEQ)
          PRINT_OP (NEQ_DBL)
          PRINT_OP (TRANS_MUL)
          PRINT_OP (MUL_TRANS)
          PRINT_OP (HERM_MUL)
          PRINT_OP (MUL_HERM)
          PRINT_OP (INCR_PREFIX)
          PRINT_OP (ROT)
          PRINT_OP (TRANS_LDIV)
          PRINT_OP (HERM_LDIV)
          PRINT_OP (POW_DBL)
          PRINT_OP (POW)
          PRINT_OP (LDIV)
          PRINT_OP (EL_MUL)
          PRINT_OP (EL_DIV)
          PRINT_OP (EL_POW)
          PRINT_OP (EL_AND)
          PRINT_OP (EL_OR)
          PRINT_OP (EL_LDIV)
          PRINT_OP (NOT_DBL)
          PRINT_OP (NOT_BOOL)
          PRINT_OP (NOT)
          PRINT_OP (UADD)
          PRINT_OP (USUB)
          PRINT_OP (USUB_DBL)
          PRINT_OP (TRANS)
          PRINT_OP (HANDLE_SIGNALS)
          PRINT_OP (HERM)
          PRINT_OP (UNARY_TRUE)
          PRINT_OP (PUSH_TRUE)
          PRINT_OP (PUSH_FALSE)
          PRINT_OP (COLON2)
          PRINT_OP (COLON3)
          PRINT_OP (COLON2_CMD)
          PRINT_OP (COLON3_CMD)
          PRINT_OP (FOR_SETUP)
          PRINT_OP (PUSH_NIL);
          PRINT_OP (THROW_IFERROBJ);
          PRINT_OP (BRAINDEAD_PRECONDITION);
          PRINT_OP (PUSH_DBL_0);
          PRINT_OP (PUSH_DBL_1);
          PRINT_OP (PUSH_DBL_2);
          PRINT_OP (ENTER_SCRIPT_FRAME);
          PRINT_OP (EXIT_SCRIPT_FRAME);
          PRINT_OP (ENTER_NESTED_FRAME);

          CASE_START (WIDE)
            wide_opext_active = true;
          CASE_END ()

          CASE_START (PUSH_FOLDED_CST) PSLOT () PSHORT () CASE_END ()
          CASE_START (SET_FOLDED_CST) PSLOT () CASE_END ()

          CASE_START (LOAD_CST)       PCHAR () CASE_END ()
          CASE_START (LOAD_CST_ALT2)  PCHAR () CASE_END ()
          CASE_START (LOAD_CST_ALT3)  PCHAR () CASE_END ()
          CASE_START (LOAD_CST_ALT4)  PCHAR () CASE_END ()
          CASE_START (LOAD_2_CST)     PCHAR () CASE_END ()
          CASE_START (POP_N_INTS)     PCHAR () CASE_END ()
          CASE_START (DUP_MOVE)       PCHAR () CASE_END ()

          CASE_START (INDEX_STRUCT_SUBCALL) PCHAR ()  PCHAR () PCHAR () PCHAR () PCHAR_AS_CHAR () CASE_END ()

          CASE_START (MUL_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (MUL_CST_DBL)    PCHAR () PCHAR () CASE_END ()
          CASE_START (DIV_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (DIV_CST_DBL)    PCHAR () PCHAR () CASE_END ()
          CASE_START (ADD_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (ADD_CST_DBL)    PCHAR () PCHAR () CASE_END ()
          CASE_START (SUB_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (SUB_CST_DBL)    PCHAR () PCHAR () CASE_END ()
          CASE_START (LE_CST)         PCHAR () PCHAR () CASE_END ()
          CASE_START (LE_CST_DBL)     PCHAR () PCHAR () CASE_END ()
          CASE_START (LE_EQ_CST)      PCHAR () PCHAR () CASE_END ()
          CASE_START (LE_EQ_CST_DBL)  PCHAR () PCHAR () CASE_END ()
          CASE_START (GR_CST)         PCHAR () PCHAR () CASE_END ()
          CASE_START (GR_CST_DBL)     PCHAR () PCHAR () CASE_END ()
          CASE_START (GR_EQ_CST)      PCHAR () PCHAR () CASE_END ()
          CASE_START (GR_EQ_CST_DBL)  PCHAR () PCHAR () CASE_END ()
          CASE_START (EQ_CST)         PCHAR () PCHAR () CASE_END ()
          CASE_START (EQ_CST_DBL)     PCHAR () PCHAR () CASE_END ()
          CASE_START (NEQ_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (NEQ_CST_DBL)    PCHAR () PCHAR () CASE_END ()
          CASE_START (POW_CST)        PCHAR () PCHAR () CASE_END ()
          CASE_START (POW_CST_DBL)    PCHAR () PCHAR () CASE_END ()

          CASE_START (PUSH_CELL)      PCHAR () PCHAR () CASE_END ()
          CASE_START (PUSH_CELL_BIG)  PINT () PINT () CASE_END ()

          CASE_START (APPEND_CELL)    PCHAR () CASE_END ()

          CASE_START (ASSIGN)                     PSLOT() CASE_END ()
          CASE_START (BIND_ANS)                   PSLOT() CASE_END ()
          CASE_START (INCR_ID_PREFIX)             PSLOT() CASE_END ()
          CASE_START (INCR_ID_POSTFIX)            PSLOT() CASE_END ()
          CASE_START (DECR_ID_PREFIX)             PSLOT() CASE_END ()
          CASE_START (DECR_ID_POSTFIX)            PSLOT() CASE_END ()
          CASE_START (INCR_ID_PREFIX_DBL)         PSLOT() CASE_END ()
          CASE_START (INCR_ID_POSTFIX_DBL)        PSLOT() CASE_END ()
          CASE_START (DECR_ID_PREFIX_DBL)         PSLOT() CASE_END ()
          CASE_START (DECR_ID_POSTFIX_DBL)        PSLOT() CASE_END ()
          CASE_START (FORCE_ASSIGN)               PSLOT() CASE_END ()
          CASE_START (PUSH_SLOT_NARGOUT1)         PSLOT() CASE_END ()
          CASE_START (PUSH_PI)                    PSLOT() CASE_END ()
          CASE_START (PUSH_I)                     PSLOT() CASE_END ()
          CASE_START (PUSH_E)                     PSLOT() CASE_END ()
          CASE_START (PUSH_SLOT_NARGOUT1_SPECIAL) PSLOT() CASE_END ()
          CASE_START (PUSH_SLOT_INDEXED)          PSLOT() CASE_END ()
          CASE_START (PUSH_FCN_HANDLE)            PSLOT() CASE_END ()
          CASE_START (PUSH_SLOT_NARGOUT0)         PSLOT() CASE_END ()
          CASE_START (SET_SLOT_TO_STACK_DEPTH)    PSLOT() CASE_END ()

          CASE_START (DISP)           PSLOT() PWSLOT() CASE_END ()
          CASE_START (PUSH_SLOT_DISP) PSLOT() PWSLOT() CASE_END ()

          CASE_START (JMP_IFDEF)                PSHORT() CASE_END ()
          CASE_START (JMP_IFNCASEMATCH)         PSHORT() CASE_END ()
          CASE_START (JMP)                      PSHORT() CASE_END ()
          CASE_START (JMP_IF)                   PSHORT() CASE_END ()
          CASE_START (JMP_IFN)                  PSHORT() CASE_END ()
          CASE_START (JMP_IF_BOOL)              PSHORT() CASE_END ()
          CASE_START (JMP_IFN_BOOL)             PSHORT() CASE_END ()
          CASE_START (FOR_COMPLEX_SETUP)        PSHORT() CASE_END ()

          CASE_START (INSTALL_FUNCTION)       PSLOT () PINT() CASE_END ()

          CASE_START (ASSIGN_COMPOUND)        PSLOT () PCHAR () CASE_END ()

          CASE_START (INDEX_ID_NARGOUT0)      PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_ID_NARGOUT1)      PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_IDNX)             PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_ID1_MAT_2D)       PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_ID1_MAT_1D)       PSLOT () PCHAR () CASE_END ()

          CASE_START (INDEX_CELL_ID_NARGOUT0) PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_CELL_ID_NARGOUT1) PSLOT () PCHAR () CASE_END ()
          CASE_START (INDEX_CELL_IDNX)        PSLOT () PCHAR () CASE_END ()

          CASE_START (INDEX_CELL_ID_NARGOUTN) PSLOT () PCHAR () PCHAR () CASE_END ()
          CASE_START (INDEX_IDN)              PSLOT () PCHAR () PCHAR () CASE_END ()

          CASE_START (SUBASSIGN_OBJ)          PCHAR () PCHAR () CASE_END ()
          CASE_START (MATRIX)                 PCHAR () PCHAR () CASE_END ()
          CASE_START (DUPN)                   PCHAR () PCHAR () CASE_END ()

          CASE_START (INDEX_ID1_MATHY_UFUN)   PCHAR () PSLOT () PCHAR () CASE_END ()

          CASE_START (INDEX_OBJ)              PCHAR () PCHAR () PWSLOT () PCHAR () PCHAR () CASE_END ()

          CASE_START (FOR_COND) PSLOT () PSHORT () CASE_END ()

          CASE_START (FOR_COMPLEX_COND) PSHORT () PWSLOT () PWSLOT () CASE_END ()

          CASE_START (INDEX_STRUCT_NARGOUTN)  PCHAR () PWSLOT () PWSLOT () CASE_END ()
          CASE_START (END_ID)                 PSLOT () PCHAR () PCHAR () CASE_END ()

          CASE_START (PUSH_SLOT_NX)           PSLOT () PCHAR () CASE_END ()
          CASE_START (PUSH_SLOT_NARGOUTN)     PSLOT () PCHAR () CASE_END ()
          CASE_START (BRAINDEAD_WARNING)      PSLOT () PCHAR () CASE_END ()
          CASE_START (SUBASSIGN_STRUCT)       PSLOT () PWSLOT () CASE_END ()

          CASE_START (SUBASSIGN_ID)         PSLOT () PCHAR () CASE_END ()
          CASE_START (SUBASSIGN_ID_MAT_1D)  PSLOT () PCHAR () CASE_END ()
          CASE_START (SUBASSIGN_ID_MAT_2D)  PSLOT () PCHAR () CASE_END ()
          CASE_START (SUBASSIGN_CELL_ID)    PSLOT () PCHAR () CASE_END ()

          CASE_START (EVAL) PCHAR () PINT () CASE_END ()

          CASE_START (PUSH_ANON_FCN_HANDLE) PINT () CASE_END ()

          CASE_START (INDEX_STRUCT_CALL)
            PCHAR ()
            PWSLOT ()
            PCHAR ()
            PCHAR_AS_CHAR ()
          CASE_END ()

          CASE_START (LOAD_FAR_CST) PINT () CASE_END ()

          CASE_START (END_OBJ) PSLOT () PCHAR () PCHAR () CASE_END ()

          CASE_START (WORDCMD_NX) PSLOT () PCHAR () CASE_END ()
          CASE_START (WORDCMD) PSLOT () PCHAR () PCHAR () CASE_END ()

          CASE_START (SET_IGNORE_OUTPUTS)
            PCHAR ()
            int nn = *p;
            PCHAR ()
            for (int i = 0; i < nn; i++)
              PCHAR ()
          CASE_END ()

          CASE_START (CLEAR_IGNORE_OUTPUTS)
            PCHAR ()
            int nn = *p;
            for (int i = 0; i < nn; i++)
              {
                PWSLOT ()
              }
          CASE_END ()

          CASE_START (END_X_N)
            PCHAR ()

            int nn = *p;
            for (int i = 0; i < nn; i++)
              {
                PCHAR ()
                PCHAR ()
                PCHAR ()
                PWSLOT ()
              }
          CASE_END ()

          CASE_START (MATRIX_UNEVEN)
            s += " TYPE";
            PCHAR ()
            int type = *p;

            if (type == 1)
              {
                s += " ROWS"; PINT ();
                s += " COLS"; PINT ();
              }
            else
              {
                if (p + 3 >= code + n)
                  error ("invalid bytecode");
                int i = chars_to_uint (p + 1);
                s += " ROWS"; PINT ();
                s += " COLS";
                for (int j = 0; j < i; j++)
                  PINT ();
              }
          CASE_END ()

          CASE_START (SUBASSIGN_CHAINED)
            PSLOT ();
            PCHAR (); // op
            PCHAR (); // nchained
            int nn = *p;
            for (int i = 0; i < nn; i++)
              {
                PCHAR ();
                PCHAR ();
              }
          CASE_END ()

          CASE_START (GLOBAL_INIT)
              p++;
              CHECK_END ();
              if (static_cast<global_type> (*p) == global_type::GLOBAL)
                s += " 'GLOBAL'";
              else if (static_cast<global_type> (*p) == global_type::PERSISTENT)
                s += " 'PERSISTENT'";

              PWSLOT ()
              PWSLOT ()

              s += " HAS-TARGET";
              PCHAR ()
              int has_target = *p;
              if (has_target)
                {
                  s += " AFTER INIT";
                  PSHORT ();
                }
          CASE_END ()

          CASE_START (ASSIGNN)
            PCHAR ()
            int n_slots = *p;
            for (int i = 0; i < n_slots; i++)
              PWSLOT ()
          CASE_END ()

          default:
            CHECK_END ();
            error ("unknown op: %d", *p);
            break;
        }
      p++;
    }

  return v_pair_row_str;
}

void
octave::print_bytecode(bytecode &bc)
{
  using std::cout;
  using std::to_string;
  using std::setw;

  unsigned char *p = bc.m_code.data ();
  int n = bc.m_code.size ();

  CHECK (bc.m_data.size () >= 2);
  cout << "metadata:\n";
  cout << "\t" << bc.m_data[0].string_value () << "\n"; // function name
  cout << "\t" << bc.m_data[1].string_value () << "\n\n"; // function type

  cout << "frame:\n";
  cout << "\t.n_return " << to_string (*p++) << "\n";
  cout << "\t.n_args " << to_string (*p++) << "\n";
  cout << "\t.n_locals " << to_string (*p++) << "\n\n";

  cout << "slots:\n";
  int idx = 0;
  for (std::string local : bc.m_ids)
    cout << setw (5) << to_string (idx++) << ": " << local << "\n";
  cout << "\n";

  cout << "source code lut:\n";
  for (auto it : bc.m_unwind_data.m_loc_entry)
    {
      cout << "\tl:" << setw (5) << it.m_line <<
              " c:" << setw (5) <<  it.m_col <<
              " ip0:" << setw (5) << it.m_ip_start <<
              " ip1:" << setw (5) << it.m_ip_end <<
              "\n";
    }

  cout << "dbg tree object:\n";
  for (auto it : bc.m_unwind_data.m_ip_to_tree)
    {
      cout << "\tip:" << it.first << " obj=" << it.second << "\n";
    }

  if (bc.m_unwind_data.m_v_nested_vars.size ())
    {
      cout << "Nested symbols table:\n";
      for (auto it : bc.m_unwind_data.m_v_nested_vars)
        {
          cout << it.m_depth << ":nth parent's slot: " << it.m_slot_parent << ", child slot: " << it.m_slot_nested << "\n";
        }
    }

  cout << "code: (n=" << n << ")\n";
  auto v_ls = opcodes_to_strings (bc);
  for (auto ls : v_ls)
    {
      cout << "\t" << setw(5) << ls.first << ": " << ls.second << "\n";
    }
}

static int pop_code_int (unsigned char *ip)
{
  unsigned int ans;
  ip -= 4;
#ifdef WORDS_BIGENDIAN
  ans = *ip++ << 24;
  ans |= *ip++ << 16;
  ans |= *ip++ << 8;
  ans |= *ip++;
#else
  ans = *ip++;
  ans |= *ip++ << 8;
  ans |= *ip++ << 16;
  ans |= *ip++ << 24;
#endif
  return ans;
}

static int pop_code_ushort (unsigned char *ip)
{
  unsigned int ans;
  ip -= 2;
#ifdef WORDS_BIGENDIAN
  ans = *ip++ << 8;
  ans |= *ip++;
#else
  ans = *ip++;
  ans |= *ip++ << 8;
#endif
  return ans;
}



// Debug functions easy to break out into in gdb. Called by __dummy_mark_1__() in Octave
extern "C" void dummy_mark_1 (void);
extern "C" void dummy_mark_2 (void);

#define POP_CODE() *ip++
#define POP_CODE_INT() (ip++,ip++,ip++,ip++,pop_code_int (ip))
#define POP_CODE_USHORT() (ip++, ip++, pop_code_ushort (ip))

#define PUSH_OV(ov) \
  do {                           \
    new (sp++) octave_value (ov);  \
  } while ((0))

#define PUSH_OVB(ovb) \
  do {                           \
    new (sp++) octave_value_vm (ovb);  \
  } while ((0))

#define PUSH_OV_VM(ov) \
  do {                           \
    new (sp++) octave_value_vm (ov);  \
  } while ((0))

#define POP() (*--sp)

#define TOP_OVB() (sp[-1]).ovb
#define SEC_OVB() (sp[-2]).ovb

#define TOP_OV_VM() (sp[-1]).ov_vm
#define SEC_OV_VM() (sp[-2]).ov_vm

#define TOP_OV() (sp[-1]).ov
#define SEC_OV() (sp[-2]).ov
#define THIRD_OV() (sp[-3]).ov
#define FOURTH_OV() (sp[-4]).ov

#define TOP() (sp[-1])
#define SEC() (sp[-2])
#define THIRD() (sp[-3])

#define STACK_SHRINK(n) sp -= n
#define STACK_GROW(n) sp += n
#define STACK_DESTROY(n)               \
  do {                                 \
    for (int iii = 0; iii < n; iii++)  \
      (*--sp).ov.~octave_value ();     \
  } while ((0))

static void stack_lift (stack_element *start, int n_elem, int n_lift)
{
  octave_value_list tmp;
  for (int i = 0; i < n_elem; i++)
    tmp.append (std::move (start[i].ov));
  for (int i = 0; i < n_elem; i++)
    start[i].ov.~octave_value ();

  // Negative n_lift means we need to erase
  for (int i = n_lift; i < 0; i++)
    {
      start[i].ov.~octave_value ();
      new (start + i) octave_value;
    }
  for (int i = 0; i < n_lift; i++)
    new (start + i) octave_value;

  for (int i = 0; i < n_elem; i++)
    new (start + n_lift + i) octave_value (std::move (tmp.xelem (i)));
}

static void append_cslist_to_ovl (octave_value_list &ovl, octave_value ov_cs)
{
  octave_value_list cslist = ov_cs.list_value (); // TODO: Wastefull copy. octave_cs_list has no const ref to m_list.
  ovl.append (cslist);
}


// Note: The function assumes the ip points to the next opcode. I.e.
// the current opcode it searches for entries for is at ip - 1.
arg_name_entry get_argname_entry (int ip, unwind_data *unwind_data)
{
  int best_match = -1;
  int best_start = -1;

  auto &entries = unwind_data->m_argname_entries;
  for (unsigned i = 0; i < entries.size (); i++)
    {
      int start = entries[i].m_ip_start;
      int end = entries[i].m_ip_end;

      if (start > (ip - 1) || end < (ip - 1))
        continue;

      if (best_match != -1)
        {
          if (best_start > start)
            continue;
        }

      best_match = i;
      best_start = start;
    }

  if (best_match == -1)
    return {};

  return entries[best_match];
}

// Expand any cs-list among the args on the stack.
// I.e. if there is e.g. a two element cs-list
// among the args, we need to expand it and move up the
// rest of the args, on the stack.
//
// Modifies sp and n_args_on_stack ...
#define EXPAND_ARGS_CSLISTS_ON_STACK \
{                                                                 \
  stack_element *pov = &sp[-n_args_on_stack];                     \
  while (pov != sp)                                               \
    {                                                             \
      if (OCTAVE_UNLIKELY (pov->ov.is_cs_list ()))                \
        {                                                         \
          int n_change = expand_cslist_inplace (pov, sp);         \
          sp += n_change;                                         \
          pov += n_change;                                        \
          n_args_on_stack += n_change;                            \
        }                                                         \
                                                                  \
      pov++;                                                      \
    }                                                             \
}

#define POP_STACK_RANGE_TO_OVL(ovl, beg, end) \
do {                                               \
  stack_element *pbeg = beg;                       \
  stack_element *pend = end;                       \
  while (pbeg != pend)                             \
    {                                              \
      if (OCTAVE_UNLIKELY (pbeg->ov.is_cs_list ()))\
        {                                          \
          append_cslist_to_ovl (ovl, pbeg->ov);    \
          pbeg->ov.~octave_value ();               \
        }                                          \
      else                                         \
        {                                          \
          ovl.append (std::move (pbeg->ov));       \
          pbeg->ov.~octave_value ();               \
        }                                          \
                                                   \
      pbeg++;                                      \
    }                                              \
  sp = beg;                                        \
                                                   \
} while (0)

#define COPY_STACK_RANGE_TO_OVL(ovl, beg, end) \
do {                                \
  stack_element *pbeg = beg;        \
  stack_element *pend = end;        \
  while (pbeg != pend)                             \
    {                                              \
      if (OCTAVE_UNLIKELY (pbeg->ov.is_cs_list ()))\
        {                                          \
          append_cslist_to_ovl (ovl, pbeg->ov);    \
        }                                          \
      else                                         \
        {                                          \
          ovl.append (pbeg->ov);                   \
        }                                          \
                                                   \
      pbeg++;                                      \
    }                                              \
} while (0)

#define COMMA ,
#define PRINT_VM_STATE(msg)                                                         \
  do {                                                                              \
    std::cout << msg;                                                               \
    std::cout << "\n";                                                              \
    std::cout << "sp  : " << sp << "\n";                                            \
    std::cout << "bsp : " << bsp << "\n";                                           \
    std::cout << "sp i: " << sp - bsp << "\n";                                      \
    std::cout << "sp ii: " << sp - m_stack << "\n";                                 \
    std::cout << "ip  : " << ip - code << "\n";                                     \
    std::cout << "code: " << code << "\n";                                          \
    std::cout << "data: " << data << "\n";                                          \
    std::cout << "ids : " << name_data << "\n";                                     \
    std::cout << "fn  : " << m_tw->get_current_stack_frame ()->fcn_name () << "\n"; \
    std::cout << "Next op: " << std::to_string (*ip) << "\n\n";                     \
  } while ((0))

#define CHECK_STACK(n) \
  do {\
    for (unsigned i = 0; i < stack_pad; i++)\
      {\
        CHECK (m_stack0[i].u == stack_magic_int);\
        CHECK (m_stack0[i + stack_size].u == stack_magic_int);\
      }\
    CHECK (sp <= m_stack + stack_size);\
    CHECK (sp + n <= m_stack + stack_size);\
    CHECK (sp >= m_stack);\
  } while (0)

#define CHECK_STACK_N(n) CHECK (sp + n <= m_stack + stack_size)

// Access the octave_base_value as subclass type of an octave_value ov
#define REP(type,ov) static_cast<type&> (const_cast<octave_base_value &> (ov.get_rep()))

#define DISPATCH() do { \
  if (OCTAVE_UNLIKELY (m_tw->vm_dbgprofecho_flag ())) /* Do we need to check for breakpoints? */\
    goto debug_check;\
  int opcode = ip[0];\
  arg0 = ip[1];\
  ip += 2;\
  goto *instr [opcode]; /* Dispatch to next instruction */\
} while ((0))

#define DISPATCH_1BYTEOP() do { \
  if (OCTAVE_UNLIKELY (m_tw->vm_dbgprofecho_flag ())) /* Do we need to check for breakpoints? */\
    goto debug_check_1b;\
  int opcode = arg0;\
  arg0 = *ip++;\
  goto *instr [opcode]; /* Dispatch to next instruction */\
} while ((0))

std::shared_ptr<vm_profiler> vm::m_vm_profiler;
bool vm::m_profiler_enabled;
bool vm::m_trace_enabled;

// These two are used for pushing true and false ov:s to the
// operand stack.
static octave_value ov_true {true};
static octave_value ov_false {false};
#if defined (M_PI)
  static octave_value ov_pi {M_PI};
#else
  // Initialized in vm::vm()
  static octave_value ov_pi;
#endif
static octave_value ov_dbl_0 {0.0};
static octave_value ov_dbl_1 {1.0};
static octave_value ov_dbl_2 {2.0};

static octave_value ov_i {Complex (0.0, 1.0)};
#if defined (M_E)
  static octave_value ov_e {M_E};
#else
  // Initialized in vm::vm()
  static octave_value ov_e;
#endif

// TODO: Push non-nil and nil ov instead of true false to make some checks
//       faster? Would they be faster?

octave_value_list
vm::execute_code (const octave_value_list &root_args, int root_nargout)
{
  // This field is set to true at each return from this function so we can
  // assure in the caller that no exception escapes the VM in some way.
  this->m_dbg_proper_return = false;

  // Array of label pointers, corresponding to opcodes by position in
  // the array. "&&" is label address, not rvalue reference.
  static const void* instr[] =
    {
      &&pop,                                               // POP,
      &&dup,                                               // DUP,
      &&load_cst,                                          // LOAD_CST,
      &&mul,                                               // MUL,
      &&div,                                               // DIV,
      &&add,                                               // ADD,
      &&sub,                                               // SUB,
      &&ret,                                               // RET,
      &&assign,                                            // ASSIGN,
      &&jmp_if,                                            // JMP_IF,
      &&jmp,                                               // JMP,
      &&jmp_ifn,                                           // JMP_IFN,
      &&push_slot_nargout0,                                // PUSH_SLOT_NARGOUT0,
      &&le,                                                // LE,
      &&le_eq,                                             // LE_EQ,
      &&gr,                                                // GR,
      &&gr_eq,                                             // GR_EQ,
      &&eq,                                                // EQ,
      &&neq,                                               // NEQ,
      &&index_id_nargout0,                                 // INDEX_ID_NARGOUT0,
      &&push_slot_indexed,                                 // PUSH_SLOT_INDEXED,
      &&pow,                                               // POW,
      &&ldiv,                                              // LDIV,
      &&el_mul,                                            // EL_MUL,
      &&el_div,                                            // EL_DIV,
      &&el_pow,                                            // EL_POW,
      &&el_and,                                            // EL_AND,
      &&el_or,                                             // EL_OR,
      &&el_ldiv,                                           // EL_LDIV,
      &&op_not,                                            // NOT,
      &&uadd,                                              // UADD,
      &&usub,                                              // USUB,
      &&trans,                                             // TRANS,
      &&herm,                                              // HERM,
      &&incr_id_prefix,                                    // INCR_ID_PREFIX,
      &&decr_id_prefix,                                    // DECR_ID_PREFIX,
      &&incr_id_postfix,                                   // INCR_ID_POSTFIX,
      &&decr_id_postfix,                                   // DECR_ID_POSTFIX,
      &&for_setup,                                         // FOR_SETUP,
      &&for_cond,                                          // FOR_COND,
      &&pop_n_ints,                                        // POP_N_INTS,
      &&push_slot_nargout1,                                // PUSH_SLOT_NARGOUT1,
      &&index_id1,                                         // INDEX_ID_NARGOUT1,
      &&push_fcn_handle,                                   // PUSH_FCN_HANDLE,
      &&colon,                                             // COLON3,
      &&colon,                                             // COLON2,
      &&colon_cmd,                                         // COLON3_CMD,
      &&colon_cmd,                                         // COLON2_CMD,
      &&push_true,                                         // PUSH_TRUE,
      &&push_false,                                        // PUSH_FALSE,
      &&unary_true,                                        // UNARY_TRUE,
      &&index_idn,                                         // INDEX_IDN,
      &&assign_n,                                          // ASSIGNN,
      &&push_slot_nargoutn,                                // PUSH_SLOT_NARGOUTN,
      &&subassign_id,                                      // SUBASSIGN_ID,
      &&end_id,                                            // END_ID,
      &&matrix,                                            // MATRIX,
      &&trans_mul,                                         // TRANS_MUL,
      &&mul_trans,                                         // MUL_TRANS,
      &&herm_mul,                                          // HERM_MUL,
      &&mul_herm,                                          // MUL_HERM,
      &&trans_ldiv,                                        // TRANS_LDIV,
      &&herm_ldiv,                                         // HERM_LDIV,
      &&wordcmd,                                           // WORDCMD,
      &&handle_signals,                                    // HANDLE_SIGNALS,
      &&push_cell,                                         // PUSH_CELL,
      &&index_cell_id0,                                    // INDEX_CELL_ID_NARGOUT0,
      &&index_cell_id1,                                    // INDEX_CELL_ID_NARGOUT1,
      &&index_cell_idn,                                    // INDEX_CELL_ID_NARGOUTN,
      &&incr_prefix,                                       // INCR_PREFIX,
      &&rot,                                               // ROT,
      &&init_global,                                       // GLOBAL_INIT,
      &&assign_compound,                                   // ASSIGN_COMPOUND,
      &&jmp_ifdef,                                         // JMP_IFDEF,
      &&switch_cmp,                                        // JMP_IFNCASEMATCH,
      &&braindead_precond,                                 // BRAINDEAD_PRECONDITION,
      &&braindead_warning,                                 // BRAINDEAD_WARNING,
      &&force_assign,                                      // FORCE_ASSIGN, // Accepts undefined rhs
      &&push_nil,                                          // PUSH_NIL,
      &&throw_iferrorobj,                                  // THROW_IFERROBJ,
      &&index_struct_n,                                    // INDEX_STRUCT_NARGOUTN,
      &&subasgn_struct,                                    // SUBASSIGN_STRUCT,
      &&subasgn_cell_id,                                   // SUBASSIGN_CELL_ID,
      &&index_obj,                                         // INDEX_OBJ,
      &&subassign_obj,                                     // SUBASSIGN_OBJ,
      &&matrix_big,                                        // MATRIX_UNEVEN,
      &&load_far_cst,                                      // LOAD_FAR_CST,
      &&end_obj,                                           // END_OBJ,
      &&set_ignore_outputs,                                // SET_IGNORE_OUTPUTS,
      &&clear_ignore_outputs,                              // CLEAR_IGNORE_OUTPUTS,
      &&subassign_chained,                                 // SUBASSIGN_CHAINED,
      &&set_slot_to_stack_depth,                           // SET_SLOT_TO_STACK_DEPTH,
      &&dupn,                                              // DUPN,
      &&debug,                                             // DEBUG,
      &&index_struct_call,                                 // INDEX_STRUCT_CALL,
      &&end_x_n,                                           // END_X_N,
      &&eval,                                              // EVAL,
      &&bind_ans,                                          // BIND_ANS,
      &&push_anon_fcn_handle,                              // PUSH_ANON_FCN_HANDLE,
      &&for_complex_setup,                                 // FOR_COMPLEX_SETUP, // opcode
      &&for_complex_cond,                                  // FOR_COMPLEX_COND,
      &&push_slot1_special,                                // PUSH_SLOT_NARGOUT1_SPECIAL,
      &&disp,                                              // DISP,
      &&push_slot_disp,                                    // PUSH_SLOT_DISP,
      &&load_cst_alt2,                                     // LOAD_CST_ALT2,
      &&load_cst_alt3,                                     // LOAD_CST_ALT3,
      &&load_cst_alt4,                                     // LOAD_CST_ALT4,
      &&load_2_cst,                                        // LOAD_2_CST,
      &&mul_dbl,                                           // MUL_DBL,
      &&add_dbl,                                           // ADD_DBL,
      &&sub_dbl,                                           // SUB_DBL,
      &&div_dbl,                                           // DIV_DBL,
      &&pow_dbl,                                           // POW_DBL,
      &&le_dbl,                                            // LE_DBL,
      &&le_eq_dbl,                                         // LE_EQ_DBL,
      &&gr_dbl,                                            // GR_DBL,
      &&gr_eq_dbl,                                         // GR_EQ_DBL,
      &&eq_dbl,                                            // EQ_DBL,
      &&neq_dbl,                                           // NEQ_DBL,
      &&index_id1_mat_1d,                                  // INDEX_ID1_MAT_1D,
      &&index_id1_mat_2d,                                  // INDEX_ID1_MAT_2D,
      &&push_pi,                                           // PUSH_PI,
      &&index_math_ufun_id1,                               // INDEX_ID1_MATHY_UFUN,
      &&subassign_id_mat_1d,                               // SUBASSIGN_ID_MAT_1D,
      &&incr_id_prefix_dbl,                                // INCR_ID_PREFIX_DBL,
      &&decr_id_prefix_dbl,                                // DECR_ID_PREFIX_DBL,
      &&incr_id_postfix_dbl,                               // INCR_ID_POSTFIX_DBL,
      &&decr_id_postfix_dbl,                               // DECR_ID_POSTFIX_DBL,
      &&push_cst_dbl_0,                                    // PUSH_DBL_0,
      &&push_cst_dbl_1,                                    // PUSH_DBL_1,
      &&push_cst_dbl_2,                                    // PUSH_DBL_2,
      &&jmp_if_bool,                                       // JMP_IF_BOOL,
      &&jmp_ifn_bool,                                      // JMP_IFN_BOOL,
      &&usub_dbl,                                          // USUB_DBL,
      &&not_dbl,                                           // NOT_DBL,
      &&not_bool,                                          // NOT_BOOL,
      &&push_folded_cst,                                   // PUSH_FOLDED_CST,
      &&set_folded_cst,                                    // SET_FOLDED_CST,
      &&wide,                                              // WIDE
      &&subassign_id_mat_2d,
      &&enter_script_frame,
      &&exit_script_frame,
      &&ret_anon,
      &&index_idnx,
      &&index_cell_idnx,
      &&push_slot_nx,
      &&ext_nargout,
      &&wordcmd_nx,
      &&anon_maybe_set_ignore_output,
      &&enter_nested_frame,
      &&install_function,
      &&dup_move,
      &&mul_cst_dbl,
      &&mul_cst,
      &&add_cst_dbl,
      &&add_cst,
      &&div_cst_dbl,
      &&div_cst,
      &&sub_cst_dbl,
      &&sub_cst,
      &&le_cst_dbl,
      &&le_cst,
      &&le_eq_cst_dbl,
      &&le_eq_cst,
      &&gr_cst_dbl,
      &&gr_cst,
      &&gr_eq_cst_dbl,
      &&gr_eq_cst,
      &&eq_cst_dbl,
      &&eq_cst,
      &&neq_cst_dbl,
      &&neq_cst,
      &&pow_cst_dbl,
      &&pow_cst,
      &&push_i,
      &&push_e,
      &&index_struct_subcall,
      &&push_cell_big,
      &&append_cell,
    };

  if (OCTAVE_UNLIKELY (m_profiler_enabled))
    {
      auto p = vm::m_vm_profiler;
      if (p)
        {
          std::string fn_name = m_data[2].string_value (); // profiler_name () querried at compile time
          p->enter_fn (fn_name, "", m_unwind_data, m_name_data, m_code);
        }
    }

#if defined (__GNUC__) && defined (__x86_64__)
  // We strongly suggest to GCC to put sp, ip and bsp in actual registers with
  // the "local register variable" extension.
  //
  // If GCC is not nudged to put these in registers, its register allocator
  // might make the VM spend quite some time pushing and popping of the C-stack.
  register int arg0 asm("r12");
  register stack_element *sp asm("r14");    // Stack pointer register
  register unsigned char *ip asm("r15");    // The instruction pointer register
  register stack_element *bsp asm("r13");   // Base stack pointer
#else
  int arg0;
  stack_element *sp;
  unsigned char *ip;
  stack_element *bsp;
#endif

  unsigned char *code; // The instruction base register

  stack_element *rsp; // Root stack pointer. Marks the beginning of the VM stack

  octave_value *data = m_data;
  std::string *name_data = m_name_data;
  unwind_data *unwind_data = m_unwind_data;

  code = m_code;
  ip = code;
  m_ip = 0;

  m_sp = m_bsp = m_rsp = sp = bsp = rsp = m_stack;

  // Read the meta data for constructing a stack frame.
  {
#define N_RETURNS() static_cast<signed char> (code[0])
#define N_ARGS() static_cast<signed char> (code[1])
#define N_LOCALS() USHORT_FROM_UCHAR_PTR (code + 2)

    int n_returns = static_cast<signed char> (*ip++);
    // n_args is negative for varargin calls
    int n_args = static_cast<signed char> (*ip++);
    int n_locals = POP_CODE_USHORT (); // Note: An arg and return can share slot

    bool is_varargin = n_args < 0;
    bool is_varargout = n_returns < 0;

    int n_root_args = root_args.length ();

    if (is_varargin)
      n_args = -n_args;
    if (OCTAVE_UNLIKELY (n_returns < 0))  // Negative for varargout and anonymous functions
      {
        if (n_returns != -128)
          n_returns = -n_returns;
        else
          n_returns = 1;
      }

    // The first return is always nargout, as a uint64
    (*sp++).u = root_nargout;

    // Construct nil octave_values for the return slots
    for (int i = 1; i < n_returns; i++)
      PUSH_OV (); // TODO: Might be an arg i.e "[a,i] = foo (i,b)"

    // Push the args onto the stack, filling their local slots
    if (!is_varargin)
      {
        int i = 0;
        for (i = 0; i < n_root_args; i++)
          PUSH_OV (root_args (i));
        // If not all args are given, fill up with nil objects
        for (; i < n_args; i++)
          PUSH_OV ();

        set_nargin (n_root_args);   // Needed for nargin function
      }
    else
      {
        // Dont push varargin arguments
        int n_args_to_push = std::min (n_args - 1, n_root_args);
        int ii = 0;
        for (ii = 0; ii < n_args_to_push; ii++)
          PUSH_OV (root_args (ii));

        // Construct missing args (if any)
        for (; ii < n_args - 1; ii++)
          PUSH_OV ();

        // The rest of the args are to be put in a cell and be put
        // in the last argument slot
        int n_varargin = n_root_args - n_args_to_push;

        if (n_varargin > 0)
          {
            Cell cell(1, n_varargin);
            int i;
            for (i = 0; i < n_varargin; i++)
              {
                cell (0, i) = root_args (ii + i);
              }
            PUSH_OV (cell);
          }
        else
          PUSH_OV (Cell (0,0)); // Empty cell into varargin's slot

        set_nargin (n_args_to_push + n_varargin);
      }
    // Construct nil octave_values for locals in their slots
    for (int i = 0; i < n_locals - n_args - n_returns; i++)
      PUSH_OV ();

    /* We do the number of args check after frame init so that the unwind is easier. */
    if (!is_varargin && n_args < n_root_args)
      {
        std::string fn_name = unwind_data->m_name;
        (*sp++).pee = new execution_exception {"error", "Octave:invalid-fun-call",
                                               fn_name + ": function called with too many inputs"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        ip++; // unwind expects ip to point to two after the opcode being executed
        goto unwind;
      }
    if (!is_varargout && root_nargout > n_returns - 1) // n_returns includes %nargout, so subtract one
      {
        std::string fn_name = unwind_data->m_name;
        (*sp++).pee = new execution_exception {"error", "Octave:invalid-fun-call",
                                               fn_name + ": function called with too many outputs"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        ip++;
        goto unwind;
      }

    m_original_lvalue_list = m_tw->lvalue_list ();
    m_tw->set_lvalue_list (nullptr);
  }

  // Go go go
  DISPATCH ();

pop:
  {
    (*--sp).ov.~octave_value ();
    DISPATCH_1BYTEOP ();
  }
dup:
  {
    new (sp) octave_value ((sp[-1]).ov);
    sp++;
    DISPATCH_1BYTEOP ();
  }
load_cst:
  {
    // The next instruction is the offset in the data.
    int offset = arg0;

    // Copy construct it into the top of the stack
    new (sp++) octave_value (data [offset]);

    DISPATCH ();
  }
mul_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_mul, mul, MUL, m_scalar_typeid)
  DISPATCH_1BYTEOP();
mul:
  MAKE_BINOP_SELFMODIFYING (binary_op::op_mul, mul_dbl, MUL_DBL)
  DISPATCH_1BYTEOP();
div_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_div, div, DIV, m_scalar_typeid)
  DISPATCH_1BYTEOP();
div:
  MAKE_BINOP_SELFMODIFYING (binary_op::op_div, div_dbl, DIV_DBL)
  DISPATCH_1BYTEOP();
add_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_add, add, ADD, m_scalar_typeid)
  DISPATCH_1BYTEOP();
add:
  MAKE_BINOP_SELFMODIFYING (binary_op::op_add, add_dbl, ADD_DBL)
  DISPATCH_1BYTEOP();
sub_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_sub, sub, SUB, m_scalar_typeid)
  DISPATCH_1BYTEOP();
sub:
  MAKE_BINOP_SELFMODIFYING (binary_op::op_sub, sub_dbl, SUB_DBL)
  DISPATCH_1BYTEOP();
ret:
  {
    // If we have any active ~/"black hole", e.g. [~] = foo() in the stack
    // the m_output_ignore_data pointer is live. We need to pop and reset
    // lvalue lists for the tree walker.
    if (OCTAVE_UNLIKELY (m_output_ignore_data))
      {
        m_output_ignore_data->pop_frame (*this);
        output_ignore_data::maybe_delete_ignore_data (*this, 0);
      }

    // We need to tell the bytecode frame we are unwinding so that it can save
    // variables on the VM stack if it is referenced from somewhere else.
    m_tw->get_current_stack_frame ()->vm_unwinds ();

    // Assert that the stack pointer is back where it should be
    assert (bsp + N_LOCALS() == sp);

    int n_returns_callee = N_RETURNS ();

    bool is_varargout = n_returns_callee < 0;
    if (OCTAVE_UNLIKELY (is_varargout))
      n_returns_callee = -n_returns_callee;
    assert (n_returns_callee > 0);

    int n_locals_callee = N_LOCALS ();

    // Destroy locals
    //
    // Note that we destroy from the bottom towards
    // the top of the stack to calls ctors in the same
    // order as the treewalker.
    int n_dtor = n_locals_callee - n_returns_callee;

    stack_element *first = sp - n_dtor;
    while (first != sp)
      {
        (*first++).ov.~octave_value ();
      }
    sp -= n_dtor;

    if (OCTAVE_UNLIKELY (is_varargout))
      {
        // Check that varargout is a cell or undefined
        octave_value &ov_vararg = sp[-1].ov;

        bool vararg_defined = ov_vararg.is_defined ();
        if (vararg_defined && !ov_vararg.iscell ())
          {
            (*sp++).pee = new execution_exception {"error","","varargout must be a cell array object"};
            (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
            goto unwind;
          }
      }

    if (OCTAVE_UNLIKELY (m_profiler_enabled))
      {
        auto p = vm::m_vm_profiler;
        if (p)
          {
            std::string fn_name = data[2].string_value (); // profiler_name () querried at compile time
            p->exit_fn (fn_name);
          }
      }

    // Are we at the root routine?
    if (bsp == rsp)
      {
        CHECK (m_output_ignore_data == nullptr); // This can't be active

        // Collect return values in octave_value_list.
        // Skip %nargout, the first value, which is an integer.
        // n_returns_callee includes %nargout, but root_nargout doesn't.

        octave_value_list ret;

        int j;
        // nargout 0 should still give one return value, if there is one
        int n_root_wanted = std::max (root_nargout, 1);

        if (is_varargout)
          {
            CHECK_PANIC(n_returns_callee >= 2);

            octave_value ov_vararg = sp[-1].ov; // varargout on the top of the stack

            bool vararg_defined = ov_vararg.is_defined ();

            for (j = 1; j < n_returns_callee - 1 && j < n_root_wanted + 1; j++)
              {
                if (OCTAVE_UNLIKELY (bsp[j].ov.is_ref ()))
                  ret.append (bsp[j].ov.ref_rep ()->deref ());
                else
                  ret.append (std::move (bsp[j].ov));
                bsp[j].ov.~octave_value ();
              }
            // Append varargout to ret
            if (vararg_defined && j < n_root_wanted + 1)
              {
                // Push the cell array elements to the stack
                Cell cell_vararg = ov_vararg.cell_value ();
                for (int i = 0; i < cell_vararg.numel () && j + i < n_root_wanted + 1; i++)
                  {
                    octave_value &arg = cell_vararg(i);
                    ret.append (std::move (arg));
                  }
              }

            // Destroy varargout and rest of return values, if any
            for (; j < n_returns_callee; j++)
              bsp[j].ov.~octave_value ();
          }
        else
          {
            for (j = 1; j < n_returns_callee && j < (n_root_wanted + 1); j++)
              {
                if (OCTAVE_UNLIKELY (bsp[j].ov.is_ref ()))
                  ret.append (bsp[j].ov.ref_rep ()->deref ());
                else
                  ret.append (std::move (bsp[j].ov));
                bsp[j].ov.~octave_value ();
              }
            // Destroy rest of return values, if any
            for (; j < n_returns_callee; j++)
              bsp[j].ov.~octave_value ();
          }

        //Note: Stack frame object popped by caller
        CHECK_STACK (0);
        this->m_dbg_proper_return = true;

        m_tw->set_lvalue_list (m_original_lvalue_list);
        return ret;
      }

    // If the root stack pointer is not the same as the base pointer,
    // we are returning from a bytecode routine to another bytecode routine,
    // so we have to restore the caller stack frame and cleanup the callee's.
    //
    // Essentially do the same thing as in the call but in reverse order.

    // The sp now points one past the last return value
    stack_element *caller_stack_end = sp - n_returns_callee;
    sp = caller_stack_end; // sp points to one past caller stack

    // The amount of return values the caller wants, as stored last on the caller stack.
    // Note that this is not necessarily the same as nargout, the amount of return values the caller
    // want the callee to produce, stored first on callee stack.
    int caller_nval_back = (*--sp).u;

    // Restore ip
    ip = (*--sp).puc;

    // Restore bsp
    bsp = (*--sp).pse;

    // Restore id names
    name_data = (*--sp).ps;

    // Restore data
    data = (*--sp).pov;

    // Restore code
    code = (*--sp).puc;

    // Restore unwind data
    unwind_data = (*--sp).pud;

    // Restore the stack pointer. The stored address is the first arg
    // on the caller stack, or where it would have been if there are no args.
    // The args were moved to the callee stack and destroyed on the caller
    // stack in the call.
    sp = sp[-1].pse;

    // We now have the object that was called on the stack, destroy it
    STACK_DESTROY (1);

    // Move the callee's return values to the top of the stack of the caller.
    // Renaming variables to keep my sanity.
    int n_args_caller_expects = caller_nval_back;
    int n_args_callee_has = n_returns_callee - 1; // Exclude %nargout
    int n_args_actually_moved = 0;

    if (OCTAVE_UNLIKELY (is_varargout))
      {
        // Expand the cell array and push the elements to the end of the callee stack
        octave_value ov_vararg = std::move (caller_stack_end[n_args_callee_has].ov);

        n_args_callee_has--; // Assume empty varargout

        bool vararg_defined = ov_vararg.is_defined ();

        if (vararg_defined)
          {
            // Push the cell array elements to the stack
            Cell cell_vararg = ov_vararg.cell_value ();
            octave_idx_type n = cell_vararg.numel ();

            octave_idx_type n_to_push;
            // Atleast one (for 'ans') and deduct the amount of args already on the stack
            n_to_push = std::max (1, n_args_caller_expects) - n_args_callee_has;
            // Can't be negative
            n_to_push = n_to_push < 0 ? 0 : n_to_push;
            // Can't push more than amount of elements in cell
            n_to_push = std::min (n , n_to_push);

            CHECK_STACK_N (n_to_push);

            int i = 0;
            for (; i < n_to_push; i++)
              {
                octave_value &arg = cell_vararg(i);
                // Construct octave_value with placement new, in the end of the callee stack
                new (&caller_stack_end[n_args_callee_has + 1 + i].ov) octave_value (std::move (arg)); // +1 for %nargout
              }

            n_args_callee_has += i;
          }
        else if (n_args_caller_expects)
          {
            // Push an empty varargout
            new (&caller_stack_end[n_args_callee_has + 1].ov) octave_value {};

            n_args_callee_has++;
          }
      }

    int n_args_to_move = std::min (n_args_caller_expects, n_args_callee_has);

    // If no return values is requested but there exists return values,
    // we need to push one to be able to write it to ans.
    if (n_args_caller_expects == 0 && n_args_callee_has)
      {
        n_args_actually_moved++;
        PUSH_OV (std::move (caller_stack_end[1].ov));
      }
    // If the callee aint returning anything, we need to push a
    // nil object, since the caller always anticipates atleast
    // one object, even for nargout == 0.
    else if (n_args_caller_expects == 0 && !n_args_callee_has)
      PUSH_OV();
    // If the stacks will overlap due to many returns, do copy via container
    else if (sp + n_args_caller_expects >= caller_stack_end)
      {
        // This pushes 'n_args_to_move' number of return values and 'n_args_caller_expects - n_args_to_move'
        // number of nils.
        copy_many_args_to_caller (sp, caller_stack_end + 1, n_args_to_move, n_args_caller_expects);
        n_args_actually_moved = n_args_caller_expects;
        sp += n_args_actually_moved;
      }
    // Move 'n_args_to_move' return value from callee to caller
    else
      {
        // If the caller wants '[a, b, ~]' and the callee has 'd e'
        // we need to push 'nil' 'd' 'e'
        for (int i = n_args_to_move; i < n_args_caller_expects; i++)
          PUSH_OV ();
        for (int i = 0; i < n_args_to_move; i++)
          {
            // Move into caller stack. Note that the order is reversed, such that
            // a b c on the callee stack becomes c b a on the caller stack.
            int idx = n_args_to_move - 1 - i;
            octave_value &arg = caller_stack_end[1 + idx].ov;

            PUSH_OV (std::move (arg));
          }
        n_args_actually_moved = n_args_caller_expects;
      }

    // Destroy the unused return values on the callee stack
    for (int i = 0; i < n_args_callee_has; i++)
      {
        int idx = n_args_callee_has - 1 - i;
        caller_stack_end[1 + idx].ov.~octave_value (); // Destroy ov in callee
      }

    // Pop the current dynamic stack frame
    std::shared_ptr<stack_frame> fp = m_tw->pop_return_stack_frame ();
    // If the pointer is not shared, stash it in a cache which is used
    // to avoid having to allocate shared pointers each frame push.
    // If it is a closure context, there might be weak pointers to it from function handles.
    if (fp.unique () && m_frame_ptr_cache.size () < 8 && ! fp->is_closure_context () 
        && fp->is_user_fcn_frame ())
      {
        fp->vm_clear_for_cache ();
        m_frame_ptr_cache.push_back (std::move (fp));
      }

    // Continue execution back in the caller
  }
  DISPATCH ();
assign:
  {
    // The next instruction is the slot number
    int slot = arg0;

    octave_value_vm &ov_rhs = TOP_OV_VM ();
    octave_value_vm &ov_lhs = bsp[slot].ov_vm;

    // Handle undefined, cs-lists, objects that need an unique call etc
    // in a separate code block to keep assign short.
    if (OCTAVE_UNLIKELY (ov_rhs.vm_need_dispatch_assign_rhs () ||
                         ov_lhs.vm_need_dispatch_assign_lhs ()))
        goto assign_dispath;

    ov_lhs = std::move (ov_rhs); // Note move

    ov_rhs.~octave_value_vm (); // Destroy the top of the stack.
    STACK_SHRINK (1);
  }
  DISPATCH();

// Note: Not an op-code. Only jumped to from assign above.
assign_dispath:
{
  // Extract the slot number again
  int slot = arg0;

  octave_value &ov_rhs = TOP_OV ();
  octave_value &ov_lhs = bsp[slot].ov;

  // If rhs is a "comma separated list" we just assign the first one.
  // E.g.:
  // a = {1,2,3};
  // b = a{:}; % This assignment
  //
  // TODO: Do some smart function in ov for this?
  //       Combine with undefined check?
  if (ov_rhs.is_cs_list ())
    {
      const octave_value_list lst = ov_rhs.list_value ();

      if (lst.empty ())
        {
          // TODO: Need id, name
          (*sp++).i = static_cast<int> (error_type::INVALID_N_EL_RHS_IN_ASSIGNMENT);
          goto unwind;
        }

      ov_rhs = lst(0);
    }

  if (ov_rhs.is_undefined ())
    {
      // TODO: Need id, name
      (*sp++).i = static_cast<int> (error_type::RHS_UNDEF_IN_ASSIGNMENT);
      goto unwind;
    }

  // If the object in the slot is the last one of it, we need
  // to call its object dtor.
  // TODO: Probably not needed since the Octave dtor will be called
  //       by the C++ dtor of ov_lhs's m_count is 0??? The assign
  //       function calls this function though ...
  ov_lhs.maybe_call_dtor ();

  if (ov_rhs.vm_need_storable_call ())
    ov_rhs.make_storable_value (); // Some types have lazy copy

  if (OCTAVE_LIKELY (!ov_lhs.is_ref ()))
    ov_lhs = std::move (ov_rhs); // Note move
  else
    ov_lhs.ref_rep ()->set_value (std::move (ov_rhs));

  STACK_DESTROY (1);
}
DISPATCH();

jmp_if_bool:
{
  octave_value_vm &ov_1 = TOP_OV_VM ();

  if (OCTAVE_UNLIKELY (ov_1.type_id () != m_bool_typeid))
    {
      // Change the specialized opcode to the generic one
      ip[-2] = static_cast<unsigned char> (INSTR::JMP_IF);
      goto jmp_if;
    }

  unsigned char b0 = arg0;
  unsigned char b1 = *ip++;

  int target = USHORT_FROM_UCHARS (b0, b1);

  octave_bool &ovb_bool = REP (octave_bool, ov_1);

  bool is_true = ovb_bool.octave_bool::is_true ();

  ov_1.~octave_value_vm ();
  STACK_SHRINK (1);

  if (is_true)
    ip = code + target;
}
DISPATCH ();

jmp_if:
  {
    octave_value &ov_1 = TOP_OV ();

    if (OCTAVE_UNLIKELY (ov_1.type_id () == m_bool_typeid))
      {
        // Change the generic opcode to the specialized one
        ip[-2] = static_cast<unsigned char> (INSTR::JMP_IF_BOOL);
        goto jmp_if_bool;
      }

    unsigned char b0 = arg0;
    unsigned char b1 = *ip++;

    int target = USHORT_FROM_UCHARS (b0, b1);

    bool is_true;
    if (ov_1.is_defined ())
      {
        try
          {
            is_true = ov_1.is_true ();
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
    else
      {
        (*sp++).i = static_cast<int> (error_type::IF_UNDEFINED);
        goto unwind;
      }

    STACK_DESTROY (1);

    if (is_true)
      ip = code + target;
  }
  DISPATCH();
jmp:
  {
    unsigned char b0 = arg0;
    unsigned char b1 = *ip++;

    int target = USHORT_FROM_UCHARS (b0, b1);
    ip = code + target;
  }
  DISPATCH ();
jmp_ifn_bool:
{
  octave_value_vm &ov_1 = TOP_OV_VM ();

  if (OCTAVE_UNLIKELY (ov_1.type_id () != m_bool_typeid))
    {
      // Change the specialized opcode to the generic one
      ip[-2] = static_cast<unsigned char> (INSTR::JMP_IFN);
      goto jmp_ifn;
    }

  unsigned char b0 = arg0;
  unsigned char b1 = *ip++;

  int target = USHORT_FROM_UCHARS (b0, b1);

  octave_bool &ovb_bool = REP (octave_bool, ov_1);

  bool is_true = ovb_bool.octave_bool::is_true ();

  ov_1.~octave_value_vm ();
  STACK_SHRINK (1);

  if (!is_true)
    ip = code + target;
}
DISPATCH ();

jmp_ifn:
  {
    octave_value &ov_1 = TOP_OV ();

    if (OCTAVE_UNLIKELY (ov_1.type_id () == m_bool_typeid))
      {
        // Change the generic opcode to the specialized one
        ip[-2] = static_cast<unsigned char> (INSTR::JMP_IFN_BOOL);
        goto jmp_ifn_bool;
      }

    unsigned char b0 = arg0;
    unsigned char b1 = *ip++;

    int target = USHORT_FROM_UCHARS (b0, b1);

    bool is_true;
    if (ov_1.is_defined ()) //10
      {
        try
          {
            is_true = ov_1.is_true ();
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
    else
      {
        (*sp++).i = static_cast<int> (error_type::IF_UNDEFINED);
        goto unwind;
      }

    STACK_DESTROY (1);

    if (!is_true)
      ip = code + target;
  }
  DISPATCH ();
push_slot_nargoutn:
  {
    // The next instruction is the slot number
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    // Handle undefined (which might be an error or function
    // call on command form) or a function object.
    if (ov.is_maybe_function ())
      goto cmd_fcn_or_undef_error;

    ip++; // nargout not needed

    // Push the value in the slot to the stack
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      PUSH_OV (ov);
    else
      PUSH_OV (ov.ref_rep ()->deref ()); // global, persistent ... need dereferencing
  }
  DISPATCH();
set_folded_cst:
{
  int slot = arg0;
  octave_cached_value *ovb = static_cast<octave_cached_value*> (bsp[slot].ovb);
  ovb->set_cached_obj (std::move (TOP_OV ()));
  STACK_DESTROY (1);
}
DISPATCH();
push_folded_cst:
  {
    int slot = arg0;
    unsigned char b0 = *ip++;
    unsigned char b1 = *ip++;

    // If the slot value is defined it is a octave_cached_value, since only
    // this opcode and SET_FOLDED_CST writes to the slot.

    bool did_it = false;
    octave_base_value *ovb = bsp[slot].ovb;
    if (ovb->is_defined ())
      {
        octave_cached_value *ovbc = static_cast<octave_cached_value *> (ovb);
        if (ovbc->cache_is_valid ())
          {
            // Use the cached value. Push it to the stack.
            PUSH_OV (ovbc->get_cached_value ());
            // Jump over the initialization code (the folded code) of the
            // cached value
            int target = USHORT_FROM_UCHARS (b0, b1);
            ip = code + target;

            did_it = true;
          }
      }

    if (! did_it)
      {
        // Put a octave_cached_value in the slot for SET_FOLDED_CST
        bsp[slot].ov = octave_value {new octave_cached_value};
      }
  }
  DISPATCH();

push_slot_nargout0:
push_slot_nargout1:
push_slot1_special:
push_slot_nx:
  {
    int slot = arg0;

    octave_base_value *ovb = bsp[slot].ovb;

    // Some ov:s need some checks before pushing
    if (OCTAVE_UNLIKELY (ovb->vm_need_dispatch_push ()))
      goto push_slot_dispatch;

    PUSH_OVB (ovb);
  }
  DISPATCH();
// This is not an op-code and is only jumped to from above opcode.
push_slot_dispatch:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    // Handle some special cases separately.
    // I.e. cmd fn calls or classdef metas.
    // Also error if no function-ish thing is found
    // in lookups.
    if (ov.is_maybe_function ())
      goto cmd_fcn_or_undef_error;

    // Push the value in the slot to the stack
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      PUSH_OV (ov);
    else
      PUSH_OV (ov.ref_rep ()->deref ()); // global, persistent ... need dereferencing
  }
  DISPATCH();

disp:
  {
    octave_value &ov = TOP_OV ();
    // 0 is magic slot number that indicates no name or always not a command
    // for this opcode.
    int slot = arg0;
    int slot_was_cmd = POP_CODE_USHORT (); // Marker for if the preceding call was a command call

    bool call_was_cmd = false;
    if (slot_was_cmd)
      {
        octave_value &ov_call_was_cmd = bsp[slot_was_cmd].ov;
        if (ov_call_was_cmd.is_defined ())
          call_was_cmd = true;
      }

    if (m_tw->statement_printing_enabled () && ov.is_defined ())
      {
        interpreter& interp = m_tw->get_interpreter ();

        if (ov.is_cs_list ())
          {
            octave_value_list ovl = ov.list_value ();

            for (int i = 0; i < ovl.length (); i++)
              {
                octave_value el_ov = ovl(i);
                // We are not printing undefined elements
                if (el_ov.is_undefined ())
                  continue;
                octave_value_list el_ovl {el_ov};
                el_ovl.stash_name_tags (string_vector ("ans"));
                m_tw->set_active_bytecode_ip (ip - code); // Needed if display calls inputname()

                try
                  {
                    interp.feval ("display", el_ovl);
                  }
                CATCH_INTERRUPT_EXCEPTION
                CATCH_INDEX_EXCEPTION
                CATCH_EXECUTION_EXCEPTION
                CATCH_BAD_ALLOC
                CATCH_EXIT_EXCEPTION
              }
          }
        else
          {
            octave_value_list ovl;
            ovl.append (ov);

            if (call_was_cmd)
              ovl.stash_name_tags (string_vector ("ans"));
            else if (slot != 0)
              ovl.stash_name_tags (string_vector (name_data[slot]));
            else
              ovl.stash_name_tags (string_vector {});

            m_tw->set_active_bytecode_ip (ip - code); // Needed if display calls inputname()

            try
              {
                interp.feval ("display", ovl);
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION

          }
      }

    STACK_DESTROY (1);
  }
  DISPATCH ();

push_slot_disp:
  {
    int slot = arg0;
    int slot_was_cmd = POP_CODE_USHORT ();
    octave_value &ov = bsp[slot].ov;
    octave_value &ov_was_cmd = bsp[slot_was_cmd].ov;

    // Handle some special cases separately.
    // I.e. cmd fn calls or classdef metas.
    // Also error if no function-ish thing is found
    // in lookups.

    // Assume that the pushed slot will not be a cmd.
    // disp will later use the ov_was_cmd slot to choose between printing
    // 'ans = ...' or 'foo = ...'
    ov_was_cmd = octave_value ();

    if (ov.is_maybe_function ())
      {
        if (ov.is_undefined ()) // class objects are defined
          ov_was_cmd = true;
        ip -= 2; // Rewind to slot so the state matches 'push_slot_nargoutn' and 'push_slot_dispatch'.
        goto cmd_fcn_or_undef_error;
      }

    // Push the value in the slot to the stack
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      PUSH_OV (ov);
    else
      PUSH_OV (ov.ref_rep ()->deref ()); // global, persistent ... need dereferencing
  }
  DISPATCH();

// Some kludge to handle the possibility of command form function calls.
cmd_fcn_or_undef_error:
  {
    int slot = arg0;
    octave_value ov = bsp[slot].ov;
    bool is_ref = ov.is_ref ();
    if (is_ref)
      ov = ov.ref_rep ()->deref ();

    // Check to opcode to see how many nargout there are.
    // Also skip ip to the end of the opcode.
    int nargout;
    bool push_classdef_metas = false;
    int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back

    INSTR opcode = static_cast<INSTR> (*(ip - 2 + wide_opcode_offset));
    if (opcode == INSTR::PUSH_SLOT_NARGOUT1 ||
        opcode == INSTR::PUSH_PI || opcode == INSTR::PUSH_I || opcode == INSTR::PUSH_E)
      nargout = 1;
    else if (opcode == INSTR::PUSH_SLOT_NARGOUT0)
      nargout = 0;
    else if (opcode == INSTR::PUSH_SLOT_NARGOUTN)
      nargout = *ip++;
    else if (opcode == INSTR::PUSH_SLOT_NARGOUT1_SPECIAL)
      {
        push_classdef_metas = true;
        nargout = 1;
      }
    else if (opcode == INSTR::PUSH_SLOT_DISP)
      {
        nargout = 0;
        ip += 2; // Skip the maybe command slot
      }
    else if (opcode == INSTR::PUSH_SLOT_NX)
      {
        nargout = bsp[0].i;
      }
    else
      PANIC ("Invalid opcode");

    bool ov_defined1 = ov.is_defined ();

    if (!ov_defined1 && ov.is_nil ())
      {
        ov = octave_value (new octave_fcn_cache (name_data[slot]));
        if (bsp[slot].ov.is_ref ())
          bsp[slot].ov.ref_rep ()->set_value (ov);
        else
          bsp[slot].ov = ov;
      }

    if (!ov_defined1 && ov.is_function_cache ())
      {
        try
          {
            octave_fcn_cache &cache = REP (octave_fcn_cache, ov);
            ov = cache.get_cached_obj ();
          }
        CATCH_EXECUTION_EXCEPTION
      }

    if (! ov.is_defined ())
      {
        (*sp++).ps = new std::string {name_data[slot]};
        (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
        goto unwind;
      }

    // When executing op-code PUSH_SLOT_NARGOUT1_SPECIAL ...
    // Essentially if we have a foo{1} where foo is a classdef
    // we need to push it for the {1} indexing.
    if (push_classdef_metas && ov.is_classdef_meta ())
      PUSH_OV (ov);
    else if (ov.is_function ())
      {
        octave_function *fcn = ov.function_value (true); //TODO: Unwind on error?

        // TODO: Bytecode call
        if (fcn)
          {

            if (fcn->is_compiled ())
              {
                octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);

                // Alot of code in this define
                PUSH_OV (ov); // Calling convention anticipates object to call on the stack.
                int n_args_on_stack = 0;
                int caller_nvalback = nargout; // Caller wants as many values returned as it wants the callee to produce
                MAKE_BYTECODE_CALL

                // Now dispatch to first instruction in the
                // called function
              }
            else
              {
              try
                {
                  m_tw->set_active_bytecode_ip (ip - code);
                  octave_value_list ovl = fcn->call (*m_tw, nargout);

                  EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ovl, nargout);
                }
              CATCH_INTERRUPT_EXCEPTION
              CATCH_INDEX_EXCEPTION
              CATCH_EXECUTION_EXCEPTION
              CATCH_BAD_ALLOC
              CATCH_EXIT_EXCEPTION
            }
          }
        else
          PUSH_OV (ov); // TODO: The walker does this. Sane?
      }
    else
      PUSH_OV (ov); // TODO: The walker does this. Sane?
  }
  DISPATCH ();
le_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_le, le, LE, m_scalar_typeid)
  DISPATCH_1BYTEOP ();
le:
  MAKE_BINOP_SELFMODIFYING (binary_op::op_lt, le_dbl, LE_DBL)
  DISPATCH_1BYTEOP ();
le_eq_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_le_eq, le_eq, LE_EQ, m_scalar_typeid)
  DISPATCH_1BYTEOP();
le_eq:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_le, le_eq_dbl, LE_EQ_DBL)
  DISPATCH_1BYTEOP();
gr_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_gr, gr, GR, m_scalar_typeid)
  DISPATCH_1BYTEOP();
gr:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_gt, gr_dbl, GR_DBL)
  DISPATCH_1BYTEOP();
gr_eq_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_gr_eq, gr_eq, GR_EQ, m_scalar_typeid)
  DISPATCH_1BYTEOP();
gr_eq:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_ge, gr_eq_dbl, GR_EQ_DBL)
  DISPATCH_1BYTEOP();
eq_dbl:
  MAKE_BINOP_SPECIALIZED(m_fn_dbl_eq, eq, EQ, m_scalar_typeid)
  DISPATCH_1BYTEOP();
eq:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_eq, eq_dbl, EQ_DBL)
  DISPATCH_1BYTEOP();
neq_dbl:
  MAKE_BINOP_SPECIALIZED(m_fn_dbl_neq, neq, NEQ, m_scalar_typeid)
  DISPATCH_1BYTEOP();
neq:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_ne, neq_dbl, NEQ_DBL)
  DISPATCH_1BYTEOP();


index_id1_mat_1d:
{
  int slot = arg0;
  ip++; // n_args_on_stack ignored

  octave_base_value *arg1 = TOP_OVB ();
  octave_value &mat = SEC_OV ();

  bool is_scalar = arg1->type_id () == m_scalar_typeid; // scalar is C "double"
  bool is_mat = mat.is_full_num_matrix ();
  // If the args have change types we need to use the generic index opcode
  if (OCTAVE_UNLIKELY (!is_scalar || !is_mat))
    {
      // Rewind ip ton_args_on_stack
      ip -= 1;
      int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
      // Change the specialized opcode to the generic one
      ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INDEX_ID_NARGOUT1);
      goto index_id1;
    }

  try
    {
      octave_scalar *arg1_double = static_cast<octave_scalar*> (arg1);

      double idx_double = arg1_double->double_value ();
      octave_idx_type idx = static_cast<octave_idx_type> (idx_double);

      if (static_cast<double> (idx) != idx_double)
        err_invalid_index (idx_double - 1, // Expects zero-indexed index
                           1,  // The 1st index has the wrong dimension
                           1); // Total amount of dimensions
      if (idx <= 0)
        err_invalid_index (idx - 1, 1, 1);

      // Arguments are one-indexed but checked_full_matrix_elem() is 0-indexed.
      octave_value ans = mat.checked_full_matrix_elem (idx - 1);
      STACK_DESTROY (2);
      PUSH_OV (std::move (ans));
    }
  CATCH_INTERRUPT_EXCEPTION
  CATCH_INDEX_EXCEPTION_WITH_NAME
  CATCH_EXECUTION_EXCEPTION
  CATCH_BAD_ALLOC
  CATCH_EXIT_EXCEPTION
}
DISPATCH();

index_id1_mat_2d:
{
  int slot = arg0;
  ip++; // n_args_on_stack ignored

  octave_base_value *arg2 = TOP_OVB (); // Collumn index
  octave_base_value *arg1 = SEC_OVB (); // Row index
  octave_value &mat = THIRD_OV ();

  bool is_scalar; // scalar as in C "double"
  is_scalar = arg1->type_id () == m_scalar_typeid;
  is_scalar = arg2->type_id () == m_scalar_typeid && is_scalar;

  bool is_mat = mat.is_full_num_matrix ();
  // If the args have change types we need to use the generic index opcode
  if (OCTAVE_UNLIKELY (!is_scalar || !is_mat))
    {
      // Rewind ip to n_args_on_stack
      ip -= 1;
      int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
      // Change the specialized opcode to the generic one
      ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INDEX_ID_NARGOUT1);
      goto index_id1;
    }

  try
    {
      octave_scalar *arg1_double = static_cast<octave_scalar*> (arg1);

      double idx1_double = arg1_double->double_value ();
      octave_idx_type idx1 = static_cast<octave_idx_type> (idx1_double);

      if (static_cast<double> (idx1) != idx1_double)
        err_invalid_index (idx1_double - 1, // Expects zero-indexed index
                           1,  // The 1st index has the wrong dimension
                           2); // Total amount of dimensions
      if (idx1 <= 0)
        err_invalid_index (idx1 - 1, 1, 2);

      octave_scalar *arg2_double = static_cast<octave_scalar*> (arg2);

      double idx2_double = arg2_double->double_value ();
      octave_idx_type idx2 = static_cast<octave_idx_type> (idx2_double);

      if (static_cast<double> (idx2) != idx2_double)
        err_invalid_index (idx2_double - 1, // Expects zero-indexed index
                           2,  // The 1st index has the wrong dimension
                           2); // Total amount of dimensions
      if (idx2 <= 0)
        err_invalid_index (idx2 - 1, 2, 2);

      // Arguments are one-indexed but checked_full_matrix_elem() is 0-indexed.
      octave_value ans = mat.checked_full_matrix_elem (idx1 - 1, idx2 - 1);
      STACK_DESTROY (3);
      PUSH_OV (std::move (ans));
    }
  CATCH_INTERRUPT_EXCEPTION
  CATCH_INDEX_EXCEPTION_WITH_NAME
  CATCH_EXECUTION_EXCEPTION
  CATCH_BAD_ALLOC
  CATCH_EXIT_EXCEPTION
}
DISPATCH();

index_math_ufun_id1:
{
  auto ufn = static_cast<octave_base_value::unary_mapper_t> (arg0);
  ip++; // slot number ignored
  ip++; // "n_args_on_stack" ignored. Always 1

  // The object to index is before the arg on the stack
  octave_value &arg = TOP_OV ();
  octave_value &ov = SEC_OV ();

  if (OCTAVE_UNLIKELY (arg.type_id () != m_scalar_typeid ||
      !ov.is_function_cache ()))
    {
      ip -= 1; // Rewind ip to n_args_on_stack
      arg0 = ip[-1]; // set arg0 to slot
      goto index_math_ufun_id1_dispatch;
    }

  // We need to check so the user has not defined some function
  // that overrides the builtin ones.
  octave_function *fcn;
  try
    {
      octave_fcn_cache &cache = REP (octave_fcn_cache, ov);
      fcn = cache.get_cached_fcn (&sp[-1], &sp[0]); // sp[-1] is the arg, sp[0] is the stack end
    }
  CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

  if (OCTAVE_UNLIKELY (!fcn->is_builtin_function ()))
    {
      ip -= 1; // Rewind ip to n_args_on_stack
      arg0 = ip[-1]; // set arg0 to slot
      goto index_math_ufun_id1_dispatch;
    }

  octave_scalar *ovb_arg = static_cast<octave_scalar*> (TOP_OVB ());

  SEC_OV () = ovb_arg->octave_scalar::map (ufn);
  STACK_DESTROY (1);
}
DISPATCH ();

push_pi:
// Specialization to push pi fast as a scalar.
//
// If the user have messed up 'pi' opcode PUSH_SLOT_NARGOUT1
// is used instead.
{
  // The next instruction is the slot number
  int slot = arg0;

  octave_value &ov = bsp[slot].ov;
  // If the slot value is not a function cache we do a
  // PUSH_SLOT_NARGOUT1 which will most likely put a
  // function cache in the slot (unless the user has done a
  // "pi = 123;" or whatever).
  if (OCTAVE_UNLIKELY (!ov.is_function_cache ()))
    {
      goto push_slot_nargout1;
    }

  // We need to check so the user has not defined some pi function
  octave_function *fcn;
  try
    {
      octave_fcn_cache &cache = REP (octave_fcn_cache, ov);
      fcn = cache.get_cached_fcn_if_fresh ();
      if (! fcn)
        fcn = cache.get_cached_fcn (static_cast<octave_value*> (nullptr), static_cast<octave_value*> (nullptr));
    }
  CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

  if (OCTAVE_UNLIKELY (fcn != m_pi_builtin_fn))
    {
      goto push_slot_nargout1;
    }

  // The user wanna push 3.1415...
  PUSH_OV (ov_pi);
}
DISPATCH();

push_i:
// Specialization to push i (the imaginary unit) fast as a scalar.
//
// If the user use i as a variable opcode PUSH_SLOT_NARGOUT1
// is used instead.
{
  int slot = arg0;

  octave_value &ov = bsp[slot].ov;
  // If the slot value is not a function cache we do a
  // PUSH_SLOT_NARGOUT1 which will most likely put a
  // function cache in the slot (unless the user has done a
  // "i = 123;" or whatever).
  if (OCTAVE_UNLIKELY (!ov.is_function_cache ()))
    {
      goto push_slot_nargout1;
    }

  // We need to check so the user has not defined some i function
  octave_function *fcn;
  try
    {
      octave_fcn_cache &cache = REP (octave_fcn_cache, ov);
      fcn = cache.get_cached_fcn_if_fresh ();
      if (! fcn)
        fcn = cache.get_cached_fcn (static_cast<octave_value*> (nullptr), static_cast<octave_value*> (nullptr));
    }
  CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

  if (OCTAVE_UNLIKELY (fcn != m_i_builtin_fn))
    {
      goto push_slot_nargout1;
    }

  // The user wanna push i ...
  PUSH_OV (ov_i);
}
DISPATCH();

push_e:
// Specialization to push e fast as a scalar.
//
// If the user use 'e' as a variable opcode PUSH_SLOT_NARGOUT1
// is used instead.
{
  int slot = arg0;

  octave_value &ov = bsp[slot].ov;
  // If the slot value is not a function cache we do a
  // PUSH_SLOT_NARGOUT1 which will most likely put a
  // function cache in the slot (unless the user has done a
  // "e = 123;" or whatever).
  if (OCTAVE_UNLIKELY (!ov.is_function_cache ()))
    {
      goto push_slot_nargout1;
    }

  // We need to check so the user has not defined some pi function
  octave_function *fcn;
  try
    {
      octave_fcn_cache &cache = REP (octave_fcn_cache, ov);
      fcn = cache.get_cached_fcn_if_fresh ();
      if (! fcn)
        fcn = cache.get_cached_fcn (static_cast<octave_value*> (nullptr), static_cast<octave_value*> (nullptr));
    }
  CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

  if (OCTAVE_UNLIKELY (fcn != m_e_builtin_fn))
    {
      goto push_slot_nargout1;
    }

  // The user wanna push e...
  PUSH_OV (ov_e);
}
DISPATCH();

  {
    // TODO: Too much code. Should be broken out?

    // Note: Beutifully interleaved if branches and goto labels
    int nargout, slot;
    bool specialization_ok;
    if (0)
      {
index_idnx:
        slot = arg0;
        nargout = bsp[0].i;
        specialization_ok = false;
      }
    else if (0)
      {
index_idn:
        slot = arg0; // Needed if we need a function lookup
        nargout = *ip++;
        specialization_ok = false;
      }
    else if (0)
      {
index_id1:
        slot = arg0;
        nargout = 1;
        specialization_ok = true;
      }
    else if (0)
      {
index_id_nargout0:
        slot = arg0;
        nargout = 0;
        specialization_ok = false;
      }
    else
      {
index_math_ufun_id1_dispatch: // Escape dispatch for index_math_ufun_id1 specialization
        slot = arg0;
        nargout = 1;
        specialization_ok = false;
      }

    int n_args_on_stack = *ip++;

    // The object to index is before the args on the stack
    octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

    switch (ov.vm_dispatch_call ())
      {
        case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
          {
            // Make an ovl with the args
            octave_value_list ovl;

            // The operands are on the top of the stack
            bool all_args_double = true;
            for (int i = n_args_on_stack - 1; i >= 0; i--)
              {
                octave_value &arg = sp[-1 - i].ov;
                int type = arg.type_id ();
                if (type != m_scalar_typeid)
                  all_args_double = false;

                if (OCTAVE_UNLIKELY (type == m_cslist_typeid))
                  ovl.append (arg.list_value ());
                else
                  ovl.append (arg); // TODO: copied, not moved
              }

            // If the ov is a "full matrix", i.e. based on octave_base_matrix,
            // and the arguments are all scalar, we modify this opcode to a
            // specialized opcode for matrix scalar indexing.
            if (nargout == 1 && all_args_double && ov.is_full_num_matrix () && specialization_ok)
              {
                if (n_args_on_stack == 1)
                  {
                    ip -= 1;
                    int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back

                    CHECK (ip[-2 + wide_opcode_offset] == static_cast<unsigned char> (INSTR::INDEX_ID_NARGOUT1));
                    ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INDEX_ID1_MAT_1D);

                    goto index_id1_mat_1d;
                  }
                else if (n_args_on_stack == 2)
                  {
                    ip -= 1;
                    int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back

                    CHECK (ip[-2 + wide_opcode_offset] == static_cast<unsigned char> (INSTR::INDEX_ID_NARGOUT1));
                    ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INDEX_ID1_MAT_2D);

                    goto index_id1_mat_2d;
                  }
              }

            octave_value_list retval;

            CHECK_PANIC (! ov.is_function () || ov.is_classdef_meta ()); // TODO: Remove

            try
              {
                m_tw->set_active_bytecode_ip (ip - code);
                retval = ov.simple_subsref ('(', ovl, nargout);
                ovl.clear ();
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION_WITH_NAME
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION

            ov = octave_value ();

            STACK_DESTROY (n_args_on_stack + 1);
            EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
          }
        break;

        case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
          {
            // It is probably a function call
            CHECK_PANIC (ov.is_nil ()); // TODO :Remove

            // Put a function cache object in the slot and in the local ov
            ov = octave_value (new octave_fcn_cache (name_data[slot]));
            if (OCTAVE_UNLIKELY (bsp[slot].ov.is_ref ()))
              bsp[slot].ov.ref_rep ()->set_value (ov);
            else
              bsp[slot].ov = ov;
          }
          // Fallthrough
        case octave_base_value::vm_call_dispatch_type::OCT_CALL:
        case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
        case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
          {
            CHECK_PANIC (ov.has_function_cache ()); // TODO :Remove

            octave_function *fcn;
            try
              {
                stack_element *first_arg = &sp[-n_args_on_stack];
                stack_element *end_arg = &sp[0];
                fcn = ov.get_cached_fcn (first_arg, end_arg);
              }
            CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

            if (! fcn)
              {
                (*sp++).ps = new std::string {name_data[slot]};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }
            else if (fcn->is_compiled ())
              {
                octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);

                // Alot of code in this define
                int caller_nvalback = nargout; // Caller wants as many values returned as it wants the callee to produce
                MAKE_BYTECODE_CALL

                // Now dispatch to first instruction in the
                // called function
              }
            else
              {
                try
                  {
                    octave_value_list ovl;// = octave_value_list::make_ovl_from_stack_range (sp - n_args_on_stack, sp);
                    //sp = sp - n_args_on_stack;
                    // The operands are on the top of the stack
                    POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                    m_tw->set_active_bytecode_ip (ip - code);
                    octave_value_list ret = fcn->call (*m_tw, nargout, ovl);

                    STACK_DESTROY (1);
                    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                  }
                CATCH_INTERRUPT_EXCEPTION
                CATCH_INDEX_EXCEPTION
                CATCH_EXECUTION_EXCEPTION
                CATCH_BAD_ALLOC
                CATCH_EXIT_EXCEPTION
              }
          }
        break;

        case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
          {
            (*sp++).i = n_args_on_stack;
            (*sp++).i = nargout;
            (*sp++).i = nargout; // "caller_nvalback". Caller wants as many values returned as it wants the callee to produce
            (*sp++).i = slot;
            goto make_nested_handle_call;
          }
      }
  }
  DISPATCH ();

push_slot_indexed:
  {
    // The next instruction is the slot number
    int slot = arg0;
    octave_value &ov = bsp[slot].ov;

    // Unlike push_slot this can't be a command function call
    // so we don't need to check if this is a function.

    // Push the value in the slot to the stack
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      PUSH_OV (ov);
    else
      PUSH_OV (ov.ref_rep ()->deref ()); // global, persistent ... need dereferencing

  }
  DISPATCH();

pow_dbl:
  MAKE_BINOP_SPECIALIZED (m_fn_dbl_pow, pow, POW, m_scalar_typeid)
  DISPATCH_1BYTEOP();
pow:
  MAKE_BINOP_SELFMODIFYING(binary_op::op_pow, pow_dbl, POW_DBL)
  DISPATCH_1BYTEOP();
ldiv:
  MAKE_BINOP(binary_op::op_ldiv)
  DISPATCH_1BYTEOP();
el_mul:
  MAKE_BINOP(binary_op::op_el_mul)
  DISPATCH_1BYTEOP();
el_div:
  MAKE_BINOP(binary_op::op_el_div)
  DISPATCH_1BYTEOP();
el_pow:
  MAKE_BINOP(binary_op::op_el_pow)
  DISPATCH_1BYTEOP();
el_and:
  MAKE_BINOP(binary_op::op_el_and)
  DISPATCH_1BYTEOP();
el_or:
  MAKE_BINOP(binary_op::op_el_or)
  DISPATCH_1BYTEOP();
el_ldiv:
  MAKE_BINOP(binary_op::op_el_ldiv)
  DISPATCH_1BYTEOP();

not_dbl:
MAKE_UNOP_SPECIALIZED (m_fn_dbl_not, op_not, NOT, m_scalar_typeid);
DISPATCH_1BYTEOP ();

not_bool:
MAKE_UNOP_SPECIALIZED (m_fn_bool_not, op_not, NOT, m_bool_typeid);
DISPATCH_1BYTEOP ();

op_not:
  {
    octave_value &ov = TOP_OV ();

    int type_id = ov.type_id ();
    if (OCTAVE_UNLIKELY (type_id == m_scalar_typeid))
      {
        // Change the generic opcode to the specialized one
        ip[-2] = static_cast<unsigned char> (INSTR::NOT_DBL);
        goto not_dbl;
      }
    else if (OCTAVE_UNLIKELY (type_id == m_bool_typeid))
      {
        // Change the generic opcode to the specialized one
        ip[-2] = static_cast<unsigned char> (INSTR::NOT_BOOL);
        goto not_bool;
      }

    try
      {
        octave_value ans = unary_op (*m_ti, octave_value::unary_op::op_not,
                                     ov);
        ov.~octave_value ();

        STACK_SHRINK (1);

        new (sp++) octave_value (std::move (ans));
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP();
uadd:
  {
    octave_value &ov = TOP_OV ();

    try
      {
        octave_value ans = unary_op (*m_ti, octave_value::unary_op::op_uplus,
                                     ov);
        ov.~octave_value ();

        STACK_SHRINK (1);

        new (sp++) octave_value (std::move (ans));
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP();

usub_dbl:
MAKE_UNOP_SPECIALIZED (m_fn_dbl_usub, usub, USUB, m_scalar_typeid);
DISPATCH_1BYTEOP ();
usub:
  {
    octave_value &ov = TOP_OV ();

    if (OCTAVE_UNLIKELY (ov.type_id () == m_scalar_typeid))
      {
        // Change the generic opcode to the specialized one
        ip[-2] = static_cast<unsigned char> (INSTR::USUB_DBL);
        goto usub_dbl;
      }

    try
      {
        octave_value ans = unary_op (*m_ti, octave_value::unary_op::op_uminus,
                                     ov);
        ov.~octave_value ();

        STACK_SHRINK (1);

        new (sp++) octave_value (std::move (ans));
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP();
trans:
  {
    octave_value &ov = TOP_OV ();

    try
      {
        octave_value ans = unary_op (*m_ti,
                                     octave_value::unary_op::op_transpose,
                                     ov);
        ov.~octave_value ();

        STACK_SHRINK (1);

        new (sp++) octave_value (std::move (ans));
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP();
herm:
  {
    octave_value &ov = TOP_OV ();

    try
      {
        octave_value ans = unary_op (*m_ti,
                                     octave_value::unary_op::op_hermitian,
                                     ov);
        ov.~octave_value ();

        STACK_SHRINK (1);

        new (sp++) octave_value (std::move (ans));
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP();

incr_id_prefix_dbl:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () != m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        // Change the specialized opcode to the generic one
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INCR_ID_PREFIX);
        goto incr_id_prefix;
      }

    octave_scalar &scalar = REP (octave_scalar, ov);
    double val = scalar.octave_scalar::double_value ();

    if (!scalar.octave_scalar::maybe_update_double (val + 1))
      ov = octave_value_factory::make (val + 1);

    PUSH_OV (ov);
  }
  DISPATCH();
incr_id_prefix:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () == m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        // Change the generic opcode to the specialized one
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INCR_ID_PREFIX_DBL);
        goto incr_id_prefix_dbl;
      }

    try
      {
        if (OCTAVE_LIKELY (!ov.is_ref ()))
          {
            ov.non_const_unary_op (octave_value::unary_op::op_incr);
            PUSH_OV (ov);
          }
        else
          {
            octave_value &ov_glb = ov.ref_rep ()->ref ();
            ov_glb.non_const_unary_op (octave_value::unary_op::op_incr);
            PUSH_OV (ov_glb);
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH();

decr_id_prefix_dbl:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () != m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::DECR_ID_PREFIX);
        goto decr_id_prefix;
      }

    octave_scalar &scalar = REP (octave_scalar, ov);
    double val = scalar.octave_scalar::double_value ();

    if (!scalar.octave_scalar::maybe_update_double (val - 1))
      ov = octave_value_factory::make (val - 1);

    PUSH_OV (ov);
  }
  DISPATCH();
decr_id_prefix:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () == m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::DECR_ID_PREFIX_DBL);
        goto decr_id_prefix_dbl;
      }

    try
      {
        if (OCTAVE_LIKELY (!ov.is_ref ()))
          {
            ov.non_const_unary_op (octave_value::unary_op::op_decr);
            PUSH_OV (ov);
          }
        else
          {
            octave_value &ov_glb = ov.ref_rep ()->ref ();
            ov_glb.non_const_unary_op (octave_value::unary_op::op_decr);
            PUSH_OV (ov_glb);
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH();
incr_id_postfix_dbl:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () != m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INCR_ID_POSTFIX);
        goto incr_id_postfix;
      }

    octave_scalar &scalar = REP (octave_scalar, ov);
    double val = scalar.octave_scalar::double_value ();

    PUSH_OV (std::move (ov));
    ov = octave_value_factory::make (val + 1);
  }
  DISPATCH();
incr_id_postfix:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () == m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::INCR_ID_POSTFIX_DBL);
        goto incr_id_postfix_dbl;
      }

    try
      {
        if (OCTAVE_LIKELY (!ov.is_ref ()))
          {
            octave_value copy = ov;
            ov.non_const_unary_op (octave_value::unary_op::op_incr);
            PUSH_OV (std::move (copy));
          }
        else
          {
            octave_value &ov_glb = ov.ref_rep ()->ref ();
            octave_value copy = ov_glb;
            ov_glb.non_const_unary_op (octave_value::unary_op::op_incr);
            PUSH_OV (std::move (copy));
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH();
decr_id_postfix_dbl:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () != m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::DECR_ID_POSTFIX);
        goto decr_id_postfix;
      }

    octave_scalar &scalar = REP (octave_scalar, ov);
    double val = scalar.octave_scalar::double_value ();

    PUSH_OV (std::move (ov));
    ov = octave_value_factory::make (val - 1);
  }
  DISPATCH();
decr_id_postfix:
  {
    int slot = arg0;

    octave_value &ov = bsp[slot].ov;

    if (ov.type_id () == m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
        ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::DECR_ID_POSTFIX_DBL);
        goto decr_id_postfix_dbl;
      }

    try
      {
        if (OCTAVE_LIKELY (!ov.is_ref ()))
          {
            octave_value copy = ov;
            ov.non_const_unary_op (octave_value::unary_op::op_decr);
            PUSH_OV (std::move (copy));
          }
        else
          {
            octave_value &ov_glb = ov.ref_rep ()->ref ();
            octave_value copy = ov_glb;
            ov_glb.non_const_unary_op (octave_value::unary_op::op_decr);
            PUSH_OV (std::move (copy));
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH();
for_setup:
  {
    octave_value &ov_range = TOP_OV ();

    octave_idx_type n = ov_range.numel();

    bool is_range = ov_range.is_range ();
    //TODO: Kludge galore. Should be refactored into some virtual call.
    if (is_range &&
        (
         ov_range.is_double_type () ||
         ov_range.is_int64_type () ||
         ov_range.is_uint64_type () ||
         ov_range.is_int32_type () ||
         ov_range.is_uint32_type () ||
         ov_range.is_int16_type () ||
         ov_range.is_uint16_type () ||
         ov_range.is_int16_type () ||
         ov_range.is_int8_type () ||
         ov_range.is_int8_type () ||
         ov_range.is_uint8_type () ||
         ov_range.is_single_type()))
      {
        ov_range = ov_range.maybe_as_trivial_range ();
      }
    else if (is_range ||
             ov_range.is_matrix_type () ||
             ov_range.iscell () ||
             ov_range.is_string () ||
             ov_range.isstruct ())
      {
        // The iteration is column wise for these, so change
        // n to the amount of columns rather then elements.
        dim_vector dv = ov_range.dims ().redim (2);
        int n_rows = dv (0);
        if (n_rows)
          n = dv(1);
        else
          n = 0; // A e.g. 0x3 sized Cell gives no iterations, not 3
      }
    else if (ov_range.is_scalar_type () || ov_range.is_undefined ())
      ;
    else
      TODO ("Unsupported for rhs type");

    // TODO: Kludgy classes.

    if (!ov_range.is_trivial_range () && is_range)
      {
        // TODO: Wasteful copy of range.
        auto range = ov_range.range_value ();
        if (math::isinf (range.limit ()) || math::isinf (range.base ()))
            warning_with_id ("Octave:infinite-loop",
                     "FOR loop limit is infinite, will stop after %"
                     OCTAVE_IDX_TYPE_FORMAT " steps", range.numel ());
      }


    // Push n to the stack
    (*sp++).i = n;
    // Push a counter to the stack, initialized so that it will
    // increment to 0.
    (*sp++).i = -1;

    // For empty rhs just assign it to lhs
    if (! n && ov_range.is_defined ())
      {
        // Slot from the for_cond that always follow a for_setup
        int slot;
        // The next opcode is in arg0, and is either WIDE or FOR_COND
        if (arg0 == static_cast<int> (INSTR::WIDE))
          {
            // Byte layout: ip[-2]:FOR_SETUP, ip[-1]:WIDE, ip[0]:FOR_COND, ip[1:2]:wide slot
            slot = USHORT_FROM_UCHAR_PTR (ip + 1);
          }
        else
          {
            // Byte layout: ip[-2]:FOR_SETUP, ip[-1]:FOR_COND, ip[0]:slot
            slot = ip[0];
          }
        try
        {
          octave_value &lhs_ov = bsp[slot].ov;
          if (!lhs_ov.is_ref ())
            lhs_ov = ov_range.storable_value ();
          else
            lhs_ov.ref_rep ()->set_value (ov_range.storable_value ());
        }
        CATCH_EXECUTION_EXCEPTION
      }
  }
DISPATCH_1BYTEOP ();

for_cond:
  {
    // Check if we should exit the loop due to e.g. ctrl-c, or handle
    // any other signals.
    try
      {
        octave_quit ();
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    // Increase counter
    TOP ().i++; // Wraps to zero first iteration

    // Check if we done all iterations
    // n is second element on the stack
    if (TOP ().i == SEC ().i)
      {
        // The after address
        unsigned char b0 = *ip++;
        unsigned char b1 = *ip++;

        int after = USHORT_FROM_UCHARS (b0, b1);

        // goto after block
        ip = code + after;
      }
    else
      {
        // Write the iteration's value to the for loop variable
        int slot = arg0;
        ip +=2; // Skip the after address

        octave_idx_type counter = TOP ().i;

        octave_value &ov_range = THIRD_OV ();
        octave_value &ov_it = bsp[slot].ov;

        if (ov_range.is_trivial_range ())
          {
            double val = REP (octave_trivial_range, ov_range).octave_trivial_range::vm_extract_forloop_double (counter);
            if (!ov_it.maybe_update_double (val))
              {
                if (OCTAVE_LIKELY (!ov_it.is_ref ()))
                  ov_it = octave_value_factory::make (val);
                else
                  ov_it.ref_rep ()->set_value (val);
              }
          }
        else if (OCTAVE_LIKELY (!ov_it.is_ref ()))
          ov_it = ov_range.vm_extract_forloop_value (counter);
        else
          ov_it.ref_rep ()->set_value (ov_range.vm_extract_forloop_value (counter));

        // The next opcode is the start of the body
      }
  }
  DISPATCH ();
pop_n_ints:
  {
    sp -= arg0;
    DISPATCH();
  }
push_fcn_handle:
  {
    int slot = arg0;

    //octave_value &fcn_cache = bsp[slot].ov;

    std::string handle_name = name_data[slot];

    if (!handle_name.size () || handle_name[0] != '@')
      TODO ("Strange handle name");

    handle_name = handle_name.substr(1);

    octave_value fcn_handle = m_tw->make_fcn_handle (handle_name);

    PUSH_OV (std::move (fcn_handle));
  }
  DISPATCH ();
colon:
  {
    bool is_for_cmd;

    // Yes, we are doing this
    if (0)
      {
colon_cmd:
        is_for_cmd = true;
      }
    else
      {
        is_for_cmd = false;
      }

    bool has_incr = false;
    if (ip[-2] == static_cast<int> (INSTR::COLON3) ||
        ip[-2] == static_cast<int> (INSTR::COLON3_CMD))
      has_incr = true;

    octave_value ret;

    if (has_incr)
      {
        octave_value &base = THIRD_OV ();
        octave_value &incr = SEC_OV ();
        octave_value &limit = TOP_OV ();

        try
        {
          ret = colon_op(base, incr, limit, is_for_cmd);
        }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION

        STACK_DESTROY (3);
      }
    else
      {
        octave_value &base = SEC_OV ();
        octave_value &limit = TOP_OV ();

        try
        {
          ret = colon_op(base, limit, is_for_cmd);
        }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION

        STACK_DESTROY (2);
      }

    PUSH_OV (std::move (ret));
  }
  DISPATCH_1BYTEOP ();

push_true:
  {
    PUSH_OV(ov_true);
  }
  DISPATCH_1BYTEOP ();
push_false:
  {
    PUSH_OV(ov_false);
  }
  DISPATCH_1BYTEOP ();
unary_true:
  {
    octave_value &op1 = TOP_OV ();

    bool is_true;

    try
      {
        is_true = op1.is_true ();
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    STACK_DESTROY (1);

    if (is_true)
      PUSH_OV (ov_true);
    else
      PUSH_OV (ov_false);

  }
  DISPATCH_1BYTEOP ();
assign_n:
  {
    int n_slots = arg0;

    int n_actual = 0;
    do
      {
        // Move operand to the local at slot in relation to base stack pointer

        octave_value &arg = (*--sp).ov;
        int slot = POP_CODE_USHORT ();
        octave_value &lhs_ov = bsp[slot].ov;


        /* Expand cs_lists */
        if (arg.is_cs_list ())
          {
            octave_value_list args = arg.list_value ();
            for (int i = 0; i < args.length (); i++)
              {
                octave_value &ov_1 = args (i);

                lhs_ov.maybe_call_dtor ();

                if (ov_1.vm_need_storable_call ())
                  ov_1.make_storable_value (); // Some types have lazy copy

                if (ov_1.is_undefined ())
                  {
                    std::string &name = name_data[slot];

                    // If the return value is ignored, undefined is OK
                    bool is_ignored = false;
                    if (name.size () >= 2 && name[0] == '%' && name[1] == '~')
                      is_ignored = true;

                    Matrix ignored;
                    octave_value tmp = m_tw->get_auto_fcn_var (stack_frame::auto_var_type::IGNORED);
                    if (tmp.is_defined ())
                      {
                        ignored = tmp.matrix_value ();

                        int n_returns = N_RETURNS ();
                        if (n_returns == -128)
                          n_returns = 1;
                        else if (n_returns < 0)
                          n_returns = -n_returns;

                        if (slot < n_returns)
                          {
                            int outputnum = n_returns - 1 - slot;

                            octave_idx_type idx = ignored.lookup (outputnum);
                            is_ignored = idx > 0 && ignored (idx - 1) == outputnum;
                          }
                      }

                    if (!is_ignored)
                      {
                        (*sp++).pee = new execution_exception {"error", "", "element number " + std::to_string (n_actual + 1) + " undefined in return list"};
                        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
                        goto unwind;
                      }
                  }

                if (OCTAVE_LIKELY (!lhs_ov.is_ref ()))
                  lhs_ov = std::move (ov_1); // Note move
                else
                  lhs_ov.ref_rep ()->set_value (ov_1);
                n_actual++;
              }
          }
        else
          {
            lhs_ov.maybe_call_dtor ();

            if (arg.vm_need_storable_call ())
              arg.make_storable_value (); // Some types have lazy copy

            if (arg.is_undefined ())
              {
                std::string &name = name_data[slot];

                // If the return value is ignored, undefined is OK
                bool is_ignored = false;
                if (name.size () >= 2 && name[0] == '%' && name[1] == '~')
                  is_ignored = true;

                Matrix ignored;
                octave_value tmp = m_tw->get_auto_fcn_var (stack_frame::auto_var_type::IGNORED);
                if (tmp.is_defined ())
                  {
                    ignored = tmp.matrix_value ();

                    int n_returns = N_RETURNS ();
                    if (n_returns == -128)
                      n_returns = 1;
                    else if (n_returns < 0)
                      n_returns = -n_returns;

                    if (slot < n_returns)
                      {
                        int outputnum = n_returns - 1 - slot;

                        octave_idx_type idx = ignored.lookup (outputnum);
                        is_ignored = idx > 0 && ignored (idx - 1) == outputnum;
                      }
                  }
                if (!is_ignored)
                  {
                    (*sp++).pee = new execution_exception {"error", "", "element number " + std::to_string (n_actual + 1) + " undefined in return list"};
                    (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
                    goto unwind;
                  }
              }

            if (OCTAVE_LIKELY (!lhs_ov.is_ref ()))
              lhs_ov = std::move (arg); // Note move
            else
              lhs_ov.ref_rep ()->set_value (arg);

            n_actual++;
          }

          arg.~octave_value (); // Destroy the operand
      }
    while (n_actual < n_slots);
  }
  DISPATCH ();

subassign_id_mat_2d:
{
  int slot = arg0;
  ip++; // nargs always two

  // The top of the stack is the rhs value
  octave_value &rhs = TOP_OV ();
  octave_value &arg2 = SEC_OV ();
  octave_value &arg1 = THIRD_OV ();
  // The ov to subassign to
  octave_value &mat_ov = bsp[slot].ov;

  int rhs_type_id = rhs.type_id ();
  int arg1_type_id = arg1.type_id ();
  int arg2_type_id = arg2.type_id ();
  int mat_type_id = mat_ov.type_id ();

  if (rhs_type_id != m_scalar_typeid || mat_type_id != m_matrix_typeid ||
    arg2_type_id != m_scalar_typeid || arg1_type_id != arg2_type_id)
  {
    // Rewind ip to the 2nd byte of the opcode
    ip -= 1;
    int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
    // Change the specialized opcode to the general one
    ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::SUBASSIGN_ID);
    goto subassign_id;
  }

  try
    {
      mat_ov.make_unique ();

      octave_scalar &rhs_scalar = REP (octave_scalar, rhs);
      octave_scalar &arg1_scalar = REP (octave_scalar, arg1);
      octave_scalar &arg2_scalar = REP (octave_scalar, arg2);

      double idx2_dbl = arg2_scalar.octave_scalar::double_value ();
      octave_idx_type idx2 = idx2_dbl - 1;
      double idx1_dbl = arg1_scalar.octave_scalar::double_value ();
      octave_idx_type idx1 = idx1_dbl - 1;
      double val = rhs_scalar.octave_scalar::double_value ();

      octave_matrix &mat_ovb = REP (octave_matrix, mat_ov);
      NDArray &arr = mat_ovb.matrix_ref ();
      // Handle out-of-bound or non-integer index in the generic opcode
      if (idx1 >= arr.rows () || idx1 < 0 ||
          idx1 != idx1_dbl - 1)
        {
          // Rewind ip to the 2nd byte of the opcode
          ip -= 1;
          goto subassign_id;
        }
      if (idx2 >= arr.cols () || idx2 < 0 ||
          idx2 != idx2_dbl - 1)
        {
          // Rewind ip to the 2nd byte of the opcode
          ip -= 1;
          goto subassign_id;
        }
      if (arr.dims ().ndims () != 2)
        {
          // Rewind ip to the 2nd byte of the opcode
          ip -= 1;
          goto subassign_id;
        }

      // The NDArray got its own m_rep that might be shared
      arr.make_unique ();

      arr.xelem (idx1, idx2) = val;
    }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

  STACK_DESTROY (3);
}
DISPATCH ();

subassign_id_mat_1d:
{
  int slot = arg0;
  ip++; // nargs always one

  // The top of the stack is the rhs value
  octave_value &rhs = TOP_OV ();
  octave_value &arg = SEC_OV ();
  // The ov to subassign to
  octave_value &mat_ov = bsp[slot].ov;

  int rhs_type_id = rhs.type_id ();
  int arg_type_id = arg.type_id ();
  int mat_type_id = mat_ov.type_id ();

  if (rhs_type_id != m_scalar_typeid || mat_type_id != m_matrix_typeid ||
    arg_type_id != m_scalar_typeid)
  {
    // Rewind ip to the 2nd byte of the opcode
    ip -= 1;
    int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back
    // Change the specialized opcode to the general one
    ip[-2 + wide_opcode_offset] = static_cast<unsigned char> (INSTR::SUBASSIGN_ID);
    goto subassign_id;
  }

  try
    {
      mat_ov.make_unique ();

      octave_scalar &rhs_scalar = REP (octave_scalar, rhs);
      octave_scalar &arg_scalar = REP (octave_scalar, arg);

      double idx_dbl = arg_scalar.octave_scalar::double_value ();
      octave_idx_type idx = idx_dbl - 1;
      double val = rhs_scalar.octave_scalar::double_value ();

      octave_matrix &mat_ovb = REP (octave_matrix, mat_ov);
      NDArray &arr = mat_ovb.matrix_ref ();
      // Handle out-of-bound or non-integer index in the generic opcode
      if (idx >= arr.numel () || idx < 0 ||
          idx != idx_dbl - 1)
        {
          // Rewind ip to the 2nd byte of the opcode
          ip -= 1;
          goto subassign_id;
        }

      // The NDArray got its own m_rep that might be shared
      arr.make_unique ();

      arr.xelem (idx) = val;
    }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

  STACK_DESTROY (2);
}
DISPATCH ();

subassign_id:
  {
    // The args to the subassign are on the operand stack
    int slot = arg0;
    int nargs = *ip++;

    // The top of the stack is the rhs value
    octave_value &rhs = TOP_OV ();
    // First argument
    stack_element *parg = sp - 1 - nargs;

    // Move the args to an ovl
    // TODO: Should actually be a move
    bool all_args_are_scalar = true;
    octave_value_list args;
    for (int i = 0; i < nargs; i++)
    {
      octave_value &arg = parg[i].ov;
      // We need to expand cs-lists
      if (arg.type_id () != m_scalar_typeid)
        all_args_are_scalar = false;
      if (arg.is_cs_list ())
        args.append (arg.list_value ());
      else
        args.append (arg);
    }

    // The ov to subassign to
    octave_value &ov = bsp[slot].ov;

    if ((nargs == 1 || nargs == 2) && all_args_are_scalar && ov.type_id () == m_matrix_typeid &&
        rhs.type_id () == m_scalar_typeid)
      {
        int wide_opcode_offset = slot < 256 ? 0 : -1; // If WIDE is used, we need to look further back

        unsigned char opcode = nargs == 1 ? static_cast<unsigned char> (INSTR::SUBASSIGN_ID_MAT_1D) : static_cast<unsigned char> (INSTR::SUBASSIGN_ID_MAT_2D);

        // If the opcode allready is SUBASSIGN_ID_MAT_1D we were sent back to
        // SUBASSIGN_ID to handle some error or edgecase, so don't go back.
        if ( ip[-3 + wide_opcode_offset] != opcode)
          {
            // Rewind ip to the 2nd byte of the opcode
            ip -= 1;
            // Change the general opcode to the specialized one
            ip[-2 + wide_opcode_offset] = opcode;
            if (nargs == 1)
              goto subassign_id_mat_1d;
            else
              goto subassign_id_mat_2d;
          }
      }

    // TODO: Room for performance improvement here maybe
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      ov.make_unique ();
    else
      ov.ref_rep ()->ref ().make_unique ();

    if (rhs.is_cs_list ())
      {
        const octave_value_list lst = rhs.list_value ();

        if (lst.empty ())
          {
            // TODO: Need id, name
            // TODO: Make execution_exception like the others instead of its own error_type
            (*sp++).i = static_cast<int> (error_type::INVALID_N_EL_RHS_IN_ASSIGNMENT);
            goto unwind;
          }

        rhs = lst(0);
      }

    // E.g. scalars do not update them self inplace
    // but create a new octave_value, so we need to
    // copy the return value to the slot.

    try
      {
        ov = ov.simple_subsasgn('(', args, rhs);
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    // Destroy the args on the operand stack aswell as rhs
    STACK_DESTROY (nargs + 1);
  }
  DISPATCH ();

end_id:
  {
    // Indexed variable
    int slot = arg0;
    // Amount of args to the index, i.e. amount of dimensions
    // being indexed.
    // E.g. foo (1,2,3) => 3
    int nargs = *ip++;
    // Index of the end, in the index, counting from 0.
    // E.g. foo (1, end, 3) => 1
    int idx = *ip++;

    octave_value ov = bsp[slot].ov;

    if (ov.is_ref ())
      ov = ov.ref_rep ()->deref ();

    if (ov.is_undefined ())
      {
        (*sp++).pee = new execution_exception {"error","","invalid use of 'end': may only be used to index existing value"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        goto unwind;
      }

    octave_value end_idx;
    if (ov.isobject ())
      {
        try
          {
            end_idx = handle_object_end (ov, idx, nargs);
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
    else
      end_idx = octave_value (ov.end_index (idx, nargs));

    PUSH_OV (std::move (end_idx));
  }
  DISPATCH ();
end_obj:
  {
    // Slot that stores the stack depth of the indexed object
    int slot = arg0;
    // Amount of args to the index, i.e. amount of dimensions
    // being indexed.
    // E.g. foo (1,2,3) => 3
    int nargs = *ip++;
    // Index of the end, in the index, counting from 0.
    // E.g. foo (1, end, 3) => 1
    int idx = *ip++;

    octave_value &stack_depth = bsp[slot].ov;
    // Indexed object
    octave_value &ov = bsp[stack_depth.int_value () - 1].ov;

    if (ov.is_undefined ())
      {
        (*sp++).pee = new execution_exception {"error","","invalid use of 'end': may only be used to index existing value"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        goto unwind;
      }

    octave_value end_idx;
    if (ov.isobject ())
      {
        try
          {
            end_idx = handle_object_end (ov, idx, nargs);
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
    else
      end_idx = octave_value (ov.end_index (idx, nargs));

    PUSH_OV (std::move (end_idx));
  }
  DISPATCH ();

end_x_n:
  {
    // Since 'end' in "foo (bar (1, end))" can refer
    // to the end of 'foo' if 'bar' is a function we
    // need to scan inner to outer after a defined
    // object to find the end of.

    int n_ids = arg0;
    int i;

    for (i = 0; i < n_ids;)
      {
        // Amount of args to the index, i.e. amount of dimensions
        // being indexed.
        // E.g. foo (1,2,3) => 3
        int nargs = *ip++;
        // Index of the end, in the index, counting from 0.
        // E.g. foo (1, end, 3) => 1
        int idx = *ip++;
        // type 0: Like 'end_id:'
        // type 1: Like 'end_obj:'
        int type = *ip++;
        // Slot that stores:
        //    the object that is being indexed for type 0
        //    the stack depth of the indexed object for type 1
        int slot = POP_CODE_USHORT ();

        octave_value ov = bsp[slot].ov;

        if (ov.is_ref ())
          ov = ov.ref_rep ()->deref ();

        // If the type is 1, the ov in the slot is the stack depth
        // of the object being indexed.
        if (type == 1)
          ov = bsp[ov.int_value () - 1].ov;

        bool is_undef = ov.is_undefined ();

        // If the outer most indexed object is not defined
        // it is an error.
        if (is_undef && i + 1 == n_ids)
          {
            (*sp++).pee = new execution_exception {"error","","invalid use of 'end': may only be used to index existing value"};
            (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
            goto unwind;
          }
        else if (is_undef)
          {
            i++;
            continue; // Look if the next outer object is defined.
          }

        octave_value end_idx;
        if (ov.isobject ())
          {
            try
              {
                end_idx = handle_object_end (ov, idx, nargs);
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION

          }
        else
          end_idx = octave_value (ov.end_index (idx, nargs));

        PUSH_OV (std::move (end_idx));
        i++;
        break;
      }

    // Skip any unread objects to index
    for (; i < n_ids; i++)
      ip += 5;
  }
  DISPATCH ();

eval:
  {
    int nargout = arg0;
    int tree_idx = POP_CODE_INT ();
    CHECK (tree_idx < 0); // Should always be negative to mark for eval. Otherwise it is debug data

    auto it = unwind_data->m_ip_to_tree.find (tree_idx);
    CHECK (it != unwind_data->m_ip_to_tree.end ());

    tree_expression *te = static_cast <tree_expression*> (it->second);

    octave_value_list retval;
    try
    {
      retval = te->evaluate_n (*m_tw, nargout);
    }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
  }
  DISPATCH ();
bind_ans:
  {
    int slot = arg0;
    octave_value &ans_on_stack = TOP_OV ();
    octave_value &ans_in_slot = bsp [slot].ov;

    if (ans_on_stack.is_defined ())
      {
        if (!ans_on_stack.is_cs_list ())
          {
            ans_in_slot.maybe_call_dtor ();
            if (ans_on_stack.vm_need_storable_call ())
              ans_on_stack.make_storable_value ();

            if (OCTAVE_LIKELY (!ans_in_slot.is_ref ()))
              ans_in_slot = std::move (ans_on_stack); // Note move
            else
              ans_in_slot.ref_rep ()->set_value (ans_on_stack);
          }
        else
          {
            // We need to recursivly expand any cs-list and assign
            // the elements one by one to ans.
            std::vector<octave_value> v_el;

            std::vector<octave_value_list> v_ovl_stack; // "recursive" stacks
            std::vector<int> v_ovl_idx_stack;

            v_ovl_stack.push_back (ans_on_stack.list_value ());
            v_ovl_idx_stack.push_back (0);

            while (true)
              {
              redo:
                octave_value_list &lst = v_ovl_stack.back ();
                int &idx = v_ovl_idx_stack.back (); // Note: reference changes in loop

                for (; idx < lst.length (); idx++)
                  {
                    octave_value ov = lst (idx);
                    if (ov.is_cs_list ())
                      {
                        idx++;
                        v_ovl_stack.push_back (ov.list_value ());
                        v_ovl_idx_stack.push_back (0);
                        goto redo;
                      }
                    else if (ov.is_defined ())
                      v_el.push_back (ov);
                  }

                v_ovl_stack.pop_back ();
                v_ovl_idx_stack.pop_back ();

                if (v_ovl_stack.size () == 0)
                  break;
              }

            // Assign all elements to ans one by one
            for (auto &ov_rhs : v_el)
              {
                ans_in_slot.maybe_call_dtor ();
                if (ov_rhs.vm_need_storable_call ())
                  ov_rhs.make_storable_value ();

                if (OCTAVE_LIKELY (!ans_in_slot.is_ref ()))
                  ans_in_slot = std::move (ov_rhs); // Note move
                else
                  ans_in_slot.ref_rep ()->set_value (ov_rhs);
              }
          }
      }

    STACK_DESTROY (1);
  }
DISPATCH ();

push_anon_fcn_handle:
{
  ip--; // Rewind ip for int macro underneath
  int tree_idx = POP_CODE_INT ();

  auto it = unwind_data->m_ip_to_tree.find (tree_idx);
  CHECK (it != unwind_data->m_ip_to_tree.end ());

  tree_anon_fcn_handle *tree_h = reinterpret_cast <tree_anon_fcn_handle*> (it->second);

  octave_value ret = m_tw->evaluate_anon_fcn_handle (*tree_h);
  octave_fcn_handle *fn_h = ret.fcn_handle_value ();
  CHECK (fn_h);
  fn_h->compile ();

  PUSH_OV (ret);
}
DISPATCH ();

for_complex_setup:
{
  octave_value &ov_rhs = TOP_OV ();
  ov_rhs.make_unique (); // TODO: Dunno if needed
  unsigned char b0 = arg0;
  unsigned char b1 = *ip++;

  int target = USHORT_FROM_UCHARS (b0, b1);

  if (ov_rhs.is_undefined ())
    {
      (*sp++).i = 1; // Need two native ints on the stack so they can be popped by the POP_N_INTS
      (*sp++).i = 2; // after the for loop body.
      ip = code + target;
      DISPATCH ();
    }

  if (!ov_rhs.isstruct ())
    {
      (*sp++).i = 1; // Need two native ints on the stack so they can be popped by the unwind.
      (*sp++).i = 2;
      (*sp++).pee = new execution_exception {"error", "", "in statement 'for [X, Y] = VAL', VAL must be a structure"};
      (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
      goto unwind;
    }

  octave_map map = ov_rhs.map_value ();
  string_vector keys = map.keys ();
  octave_idx_type n = keys.numel ();

  // Push n to the stack
  (*sp++).i = n;
  // Push a counter to the stack, initialized so that it will
  // increment to 0.
  (*sp++).i = -1;
}
DISPATCH ();

for_complex_cond:
{
  // Increase counter
  TOP ().i++; // Wraps to zero first iteration

  // Check if we done all iterations
  // n is second element on the stack
  if (TOP ().i == SEC ().i)
    {
      // The after address
      unsigned char b0 = arg0;
      unsigned char b1 = *ip++;

      int after = USHORT_FROM_UCHARS (b0, b1);

      // goto after block
      ip = code + after;
    }
  else
    {
      ip++; // Skip 2nd part of afteraddress
      int slot_key = POP_CODE_USHORT ();
      int slot_value = POP_CODE_USHORT ();
      octave_idx_type counter = TOP ().i;

      octave_value &ov_rhs = THIRD_OV (); // This is always a struct
      octave_value &ov_key = bsp[slot_key].ov;
      octave_value &ov_val = bsp[slot_value].ov;

      // TODO: Abit wasteful copying map_value () each time but whatever
      //       who uses complex for loops anyways.
      std::string key = ov_rhs.map_value ().keys () [counter];
      const Cell val_lst = ov_rhs.map_value ().contents (key);

      octave_idx_type n = val_lst.numel ();
      octave_value val = (n == 1) ? val_lst(0) : octave_value (val_lst);

      if (counter == 0)
        {
          ov_val.maybe_call_dtor (); // The first iteration these could be class objects ...
          ov_key.maybe_call_dtor ();
        }

      val.make_unique (); // TODO: Dunno if needed

      if (ov_val.is_ref ())
        ov_val.ref_rep ()->set_value (val);
      else
        ov_val = val;

      if (ov_val.is_ref ())
        ov_key.ref_rep ()->set_value (key);
      else
        ov_key = key;
    }
}
DISPATCH ();

/* For dynamic m*n matrix where m and n < 256 */
matrix:
  {
    int nrows = arg0;
    int ncols = *ip++;
    int n_el = nrows * ncols;

    // The first element is down the stack
    // and the last element is at the top.
    stack_element *first_arg = &sp[-n_el];

    // The stack pointer is pointing to the first unused
    // stack position, so it is the end pointer.
    stack_element *end_arg = sp;

    try
      {
        tm_const tmp (first_arg, end_arg, ncols, *m_tw);

        octave_value &&ov = tmp.concat (' ');

        STACK_DESTROY (n_el);

        PUSH_OV (ov);
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH ();
matrix_big:
  {
    int type = arg0;

    /* type 0 indicates a matrix that has unequal length of the rows.
     *
     * Any other value than zero indicates a big "rectangle" matrix
     * with more than 255 elements in a row or column. */
    if (type == 0)
      {
        int nrows = POP_CODE_INT ();

        std::vector<int> length_rows;

        int n_el = 0;
        for (int i = 0; i < nrows; i++)
          {
            int row_length = POP_CODE_INT ();
            length_rows.push_back (row_length);
            n_el += row_length;
          }

        // The first element is down the stack
        // and the last element is at the top.
        stack_element *first_arg = &sp[-n_el];

        // The stack pointer is pointing to the first unused
        // stack position, so it is the end pointer.
        stack_element *end_arg = sp;

        try
          {
            tm_const tmp (first_arg, end_arg, length_rows, *m_tw);

            octave_value &&ov = tmp.concat (' ');

            STACK_DESTROY (n_el);

            PUSH_OV (ov);
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
    else
      {
        int nrows = POP_CODE_INT ();
        int ncols = POP_CODE_INT ();
        int n_el = nrows * ncols;

        // The first element is down the stack
        // and the last element is at the top.
        stack_element *first_arg = &sp[-n_el];

        // The stack pointer is pointing to the first unused
        // stack position, so it is the end pointer.
        stack_element *end_arg = sp;

        try
          {
            tm_const tmp (first_arg, end_arg, ncols, *m_tw);

            octave_value &&ov = tmp.concat (' ');

            STACK_DESTROY (n_el);

            PUSH_OV (ov);
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
  }
  DISPATCH ();
trans_mul:
  MAKE_BINOP(compound_binary_op::op_trans_mul)
  DISPATCH_1BYTEOP();
mul_trans:
  MAKE_BINOP(compound_binary_op::op_mul_trans)
  DISPATCH_1BYTEOP();
herm_mul:
  MAKE_BINOP(compound_binary_op::op_herm_mul)
  DISPATCH_1BYTEOP();
mul_herm:
  MAKE_BINOP(compound_binary_op::op_mul_herm)
  DISPATCH_1BYTEOP();
trans_ldiv:
  MAKE_BINOP(compound_binary_op::op_trans_ldiv)
  DISPATCH_1BYTEOP();
herm_ldiv:
  MAKE_BINOP(compound_binary_op::op_herm_ldiv)
  DISPATCH_1BYTEOP();

  {
    int slot; // Needed if we need a function lookup
    int nargout;
    int n_args_on_stack;

    if (0)
      {
wordcmd_nx:
        slot = arg0;
        nargout = bsp[0].i;
        n_args_on_stack = *ip++;
      }
    else if (0)
      {
wordcmd:
        slot = arg0;
        nargout = *ip++;
        n_args_on_stack = *ip++;
      }

    // The object to index is before the args on the stack
    octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

    switch (ov.vm_dispatch_call ())
      {
        case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
          {
            CHECK_PANIC (ov.is_nil ()); // TODO: Remove

            // Put a function cache object in the slot and in the local ov
            ov = octave_value (new octave_fcn_cache (name_data[slot]));
            if (bsp[slot].ov.is_ref ())
              bsp[slot].ov.ref_rep ()->set_value (ov);
            else
              bsp[slot].ov = ov;
          }
          // Fallthrough
        case octave_base_value::vm_call_dispatch_type::OCT_CALL:
        case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
        case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
          {
            octave_function *fcn;
            try
              {
                stack_element *first_arg = &sp[-n_args_on_stack];
                stack_element *end_arg = &sp[0];
                fcn = ov.get_cached_fcn (first_arg, end_arg);
              }
            CATCH_EXECUTION_EXCEPTION

            if (! fcn)
              {
                (*sp++).ps = new std::string {name_data[slot]};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }

            if (fcn->is_compiled ())
              {
                octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);
                // Alot of code in this define
                int caller_nvalback = nargout; // Caller wants as many values returned as it wants the callee to produce
                MAKE_BYTECODE_CALL

                // Now dispatch to first instruction in the
                // called function
              }
            else
              {

                octave_value_list ovl;
                // The operands are on the top of the stack
                POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                try
                  {
                    m_tw->set_active_bytecode_ip (ip - code);
                    octave_value_list ret = fcn->call (*m_tw, nargout, ovl);

                    STACK_DESTROY (1);
                    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                  }
                CATCH_INTERRUPT_EXCEPTION
                CATCH_INDEX_EXCEPTION_WITH_NAME
                CATCH_EXECUTION_EXCEPTION
                CATCH_BAD_ALLOC
                CATCH_EXIT_EXCEPTION

              }
          }
          break;

        case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
        case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
          PANIC ("Invalid dispatch");
      }
  }
  DISPATCH ();
handle_signals:
  {
    // Check if there is any signal to handle
    try
      {
        octave_quit ();
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH_1BYTEOP ();
push_cst_dbl_0:
{
  PUSH_OV (ov_dbl_0);
}
DISPATCH_1BYTEOP ();
push_cst_dbl_1:
{
  PUSH_OV (ov_dbl_1);
}
DISPATCH_1BYTEOP ();
push_cst_dbl_2:
{
  PUSH_OV (ov_dbl_2);
}
DISPATCH_1BYTEOP ();

  // The PUSH_CELL and PUSH_CELL_BIG opcodes pushes a cell to the stack which later code will assign
  // elements to with APPEND_CELL. Two counters for the active column and row are also pushed.
  {
    int n_rows;
    int n_cols;

    if (0)
      {
push_cell:
        n_rows = arg0;
        n_cols = POP_CODE ();
      }
    else if (0)
      {
push_cell_big:
        ip--; // Rewind ip so that it points to the first byte after the PUSH_CELL_BIG opcode
        n_rows = POP_CODE_INT ();
        n_cols = POP_CODE_INT ();
      }

    // The size is a guess. In the end, the size might differ.
    Cell cell(n_rows, n_cols);

    PUSH_OV (cell);

    // The APPEND_CELL opcodes need to keep track of which row and column
    // they are supposed to add to, since any element can be a cs-list,
    // and the index to assign to can't be statically decided.
    PUSH_OV (new octave_int64_scalar {});
    PUSH_OV (new octave_int64_scalar {});
  }
  DISPATCH ();

append_cell:
{
  // The stack looks like this:
  // top: Element to add
  //  -1: Row counter
  //  -2: Column counter
  //  -3: The cell to add elements to

  // Essentially there is an APPEND_CELL opcode after
  // each element argument. The last APPEND_CELL in a row
  // has arg0 set to a number to indicate if it is the last
  // column in the row, with distinction between:
  //
  // a middle row == 1
  // the last row of many == 2
  // the last row of one == 3
  // the last row of many == 4
  //
  // This is needed since the first row sets how many columns the
  // other rows need to have and the last rows need to pop the two
  // counters on the stack.

  // Note that 'b = {}; c = {b{:}, b{:}}" makes c size 2x0
  // while 'b = {}; c = {b{:}}" makes c size 0x0

  int last = arg0;

  // The element we need to insert into the cell
  octave_value ov = std::move (TOP_OV ());
  STACK_SHRINK (1);

  // The cell we are adding the element to
  octave_value &ov_cell = THIRD_OV (); 
  octave_cell &ovb_cell = REP (octave_cell, ov_cell);

  Cell &cell = ovb_cell.octave_cell::matrix_ref ();

  octave_idx_type n_rows = cell.rows ();
  octave_idx_type n_cols = cell.cols ();

  // The column counter
  octave_value &ov_i_col = SEC_OV ();
  octave_int64_scalar &ovb_i_col = REP (octave_int64_scalar, ov_i_col);
  auto &i_col = ovb_i_col.octave_int64_scalar::scalar_ref ();

  octave_idx_type i_col_idx = i_col;

  // The row counter
  octave_value &ov_i_row = TOP_OV ();
  octave_int64_scalar &ovb_i_row = REP (octave_int64_scalar, ov_i_row);
  auto &i_row = ovb_i_row.octave_int64_scalar::scalar_ref ();

  octave_idx_type i_row_idx = i_row;

  if (ov.is_cs_list ())
    {
      octave_value_list ovl = ov.list_value ();
      octave_idx_type n = ovl.length ();

      // If we are operating on the first row, increase its size if we
      // are about to overflow.
      if (i_row_idx == 0 && i_col_idx + n > n_cols)
        {
          cell.resize (dim_vector (n_rows, i_col_idx + n));
          n_cols = i_col_idx + n;
        }
      
      // If there is room in the row, insert the elements into it.
      // Note that if there is no room, no element will be added to the cell,
      // there will be an error after the row's last element's arg is executed.
      // I.e. all the arg expressions in the row are always executed before
      // the error.
      if (i_col_idx + n <= n_cols)
        {
          // Insert the elements of the cs-list into to the row of the cell.
          for (octave_idx_type i = 0; i < n; i++)
            cell (i_row_idx, i_col_idx + i) = ovl (i);
        }

      i_col += n;
      i_col_idx += n;
    }
  else if (ov.is_defined ())
    {
      // If we are operating on the first row, increase its size if we
      // are about to overflow.
      if (i_row_idx == 0 && i_col_idx >= n_cols)
        {
          cell.resize (dim_vector (1, i_col_idx + 1));
          n_cols++;
        }

      // If there is room in the row, insert the element into it.
      // Note that if there is no room, no element will be added to the cell,
      // there will be an error after the row's last element's arg is executed.
      // I.e. all the arg expressions in the row are always executed before
      // the error.
      if (i_col_idx < n_cols)
        cell (i_row_idx, i_col_idx) = ov;

      i_col = i_col + static_cast<octave_int64> (1);
      i_col_idx++;
    }
  else
    {
      ; // If the arg is undefined, nothing is added to the row in the cell.
    }

  if (last == 1) // Last element in a middle row in a cell with multiple rows.
    {
      // The amount of columns in a row has to match the first row's.
      if (i_col_idx && i_col_idx != n_cols)
        {
          (*sp++).pee = new execution_exception {"error","","number of columns must match"};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }

      // Prepare for APPEND_CELL to operate on the next row.
      i_row +=  i_col_idx ? 1L : 0; // Only advance row idx if something was inserted.
      i_col = 0;
    }
  else if (last == 2) // Last element in the last row in a cell with multiple rows.
    {
      // The amount of columns in a row has to match the first row's unless
      // the amount of columns in the current row is zero.
      if (i_col_idx && i_col_idx != n_cols)
        {
          (*sp++).pee = new execution_exception {"error","","number of columns must match"};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }

      if (i_col_idx)
        i_row_idx += 1; // If this row was not empty
      else if (n_cols == 0)
        i_row_idx += 1; // If this row was empty and is supposed to be empty.

      // If all the args for a row were empty, the next row's args were inserted into the empty row,
      // so there might be trailing empty rows that we need to remove.
      if (i_row_idx != n_rows)
        {
          cell.resize (dim_vector (i_row_idx, n_cols));
        }

      // Destroy the col and row counters
      STACK_DESTROY (2);
      // The cell is now on the top of the stack
    }
  else if (last == 3) // Last element in row in a cell with one row
    {
      // If a smaller number of columns were inserted than there are arguments
      // (the amounts of args sets the initial size) we need to shrink the cell
      if (i_col_idx < n_cols)
        {
          // If the row is empty, the resulting cell should be 0x0.
          //   "b = {}; c = {b{:}}" yields c as a 0x0 cell
          // but:
          //   "b = {}; c = {b{:}; b{:}}" yields c as a 2x0 cell
          cell.resize (dim_vector (i_col_idx ? 1 : 0, i_col_idx));
        }

      // Destroy the col and row counters
      STACK_DESTROY (2);
      // The cell is now on the top of the stack
    }
  else if (last == 4) // Last element in the first row, more than one row total
    {
      // If a smaller number of columns were inserted than there are arguments
      // (the amounts of args sets the initial size) we need to shrink the cell
      if (i_col_idx < n_cols)
        {
          cell.resize (dim_vector (n_rows, i_col_idx));
        }

      // Prepare for APPEND_CELL to operate on the next row
      i_col = 0;
      // Always advance to next row, even if first row was empty since
      // if the first row was empty, all rows need to be empty.
      i_row += 1L;
    }
}
DISPATCH ();

  {
    // TODO: Too much code. Should be broken out?
    // Something made sp not be in r15.

    int nargout, slot;
    if (0)
      {
index_cell_idnx:
        slot = arg0; // Needed if we need a function lookup
        nargout = bsp[0].i;
      }
    else if (0)
      {
index_cell_idn:
        slot = arg0; // Needed if we need a function lookup
        nargout = *ip++;
      }
    else if (0)
index_cell_id1:
      {
        slot = arg0;
        nargout = 1;
      }
    else if (0)
index_cell_id0:
      {
        slot = arg0;
        nargout = 0;
      }

    int n_args_on_stack = *ip++;

    // The object to index is before the args on the stack
    octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

    switch (ov.vm_dispatch_call ())
      {
        case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
          {
            std::list<octave_value_list> idx; // TODO: mallocs!

            // Make an ovl with the args
            octave_value_list ovl;
            // The operands are on the top of the stack
            POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

            idx.push_back(ovl);

            // TODO: subsref might throw index error
            octave_value_list retval;

            try
              {
                m_tw->set_active_bytecode_ip (ip - code);
                retval = ov.subsref("{", idx, nargout);
                idx.clear ();
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION_WITH_NAME
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION

            bool is_fcn = (retval.length () ?
                            retval(0).is_function() : false);

            // "FIXME: when can the following happen?  In what case does indexing
            //  result in a value that is a function?  Classdef method calls?
            //  Something else?"

            if (OCTAVE_LIKELY (!is_fcn))
              {
                idx.clear ();
                // TODO: Necessary? I guess it might trigger dtors
                // or something?
                ov = octave_value ();
              }
            else
              {
                octave_value val = retval(0);
                octave_function *fcn = val.function_value (true);

                if (fcn)
                  {
                    octave_value_list final_args;

                    if (! idx.empty ())
                      final_args = idx.front ();

                    try
                      {
                        m_tw->set_active_bytecode_ip (ip - code);
                        retval = fcn->call (*m_tw, nargout, final_args);
                      }
                    CATCH_INTERRUPT_EXCEPTION
                    CATCH_INDEX_EXCEPTION
                    CATCH_EXECUTION_EXCEPTION
                    CATCH_BAD_ALLOC
                    CATCH_EXIT_EXCEPTION
                  }

                idx.clear ();
                ov = octave_value ();
                val = octave_value ();
              }

            STACK_DESTROY (1);
            EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
          }
          break;

        case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
          {
            // Put a function cache object in the slot and in the local ov
            ov = octave_value (new octave_fcn_cache (name_data[slot]));
            if (bsp[slot].ov.is_ref ())
              bsp[slot].ov.ref_rep ()->set_value (ov);
            else
              bsp[slot].ov = ov;
          }
          // Fallthrough
        case octave_base_value::vm_call_dispatch_type::OCT_CALL:
        case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
        case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
          {
            octave_function *fcn;
            try
              {
                stack_element *first_arg = &sp[-n_args_on_stack];
                stack_element *end_arg = &sp[0];
                fcn = ov.get_cached_fcn (first_arg, end_arg);
              }
            CATCH_EXECUTION_EXCEPTION

            if (! fcn)
              {
                (*sp++).ps = new std::string {name_data[slot]};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }

            if (fcn->is_compiled ())
              {
                octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);
                // Alot of code in this define
                int caller_nvalback = nargout; // Caller wants as many values returned as it wants the callee to produce
                MAKE_BYTECODE_CALL

                // Now dispatch to first instruction in the
                // called function
              }
            else
              {
                // Make an ovl with the args
                octave_value_list ovl;
                // The operands are on the top of the stack
                POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                try
                  {
                    m_tw->set_active_bytecode_ip (ip - code);
                    octave_value_list ret = fcn->call (*m_tw, nargout, ovl);

                    STACK_DESTROY (1);
                    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                  }
                CATCH_INTERRUPT_EXCEPTION
                CATCH_INDEX_EXCEPTION_WITH_NAME
                CATCH_EXECUTION_EXCEPTION
                CATCH_BAD_ALLOC
                CATCH_EXIT_EXCEPTION

              }
          }
          break;

        case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
          {
            (*sp++).i = n_args_on_stack;
            (*sp++).i = nargout;
            (*sp++).i = nargout; // "caller_nvalback". Caller wants as many values returned as it wants the callee to produce
            (*sp++).i = slot;
            goto make_nested_handle_call;
          }
      }
  }
  DISPATCH ();

incr_prefix:
  {
    octave_value &ov = TOP_OV ();
    // Inplace
    ov.non_const_unary_op (octave_value::unary_op::op_incr);
  }
  DISPATCH_1BYTEOP ();

rot:
  {
    octave_value top_ov = TOP_OV ();
    octave_value sec_ov = SEC_OV ();
    STACK_DESTROY (2);
    PUSH_OV (top_ov);
    PUSH_OV (sec_ov);
  }
  DISPATCH_1BYTEOP ();

varargin_call:
  {
    // We jump to here when a bytecode call notices it is
    // calling a function with varargin.
    //
    // Continue where we left off. Restore temp variables from the stack.

    octave_user_function *usr_fcn = static_cast<octave_user_function *> (sp[0].pv);

    int n_returns_callee = static_cast<signed char> (ip[-4]);
    if (OCTAVE_UNLIKELY (n_returns_callee < 0))
      {
        if (n_returns_callee == -128) /* Anonymous function */
          n_returns_callee = 1;
        else
          n_returns_callee = -n_returns_callee;
      }
    int n_args_callee = -static_cast<signed char> (ip[-3]); // Note: Minus
    int n_locals_callee = USHORT_FROM_UCHAR_PTR (ip - 2);

    int nargout = sp[-1].i;

    // Recreate first arg and n_args_on_stack
    // from the stack
    stack_element *first_arg = sp[-9].pse;
    int n_args_on_stack = (sp - 9) - first_arg;

    // Construct return values - note nargout
    // is allready pushed as a uint64
    for (int i = 1; i < n_returns_callee; i++)
      PUSH_OV ();

    int n_args_before_varargin =
      std::min (n_args_callee - 1,
                n_args_on_stack);
    // Move the args to the new stack, except varargin
    //
    // We need to expand any cs-list, but only until the next
    // argument would be in varargin. Those need to end up
    // in the varargin cell array.
    int ii;
    int n_args_on_callee_stack = 0;
    octave_value_list cs_args;
    int cs_args_idx = 0;
    for (ii = 0; ii < n_args_before_varargin; ii++)
      {
        octave_value &arg = first_arg[ii].ov;
        if (arg.is_cs_list ())
          {
            cs_args = arg.list_value ();
            cs_args_idx = 0;
            for (int j = 0; j < cs_args.length ()
                            && n_args_on_callee_stack < n_args_callee - 1; j++)
              {
                PUSH_OV (cs_args (j));
                n_args_on_callee_stack++;
                cs_args_idx++;
              }
          }
        else
          {
            PUSH_OV (std::move (arg));
            n_args_on_callee_stack++;
          }

        // Destroy the args
        first_arg[ii].ov.~octave_value ();
      }
      // TODO: Expand cl_list? Smarter way? Do it in beginning ...

    // Construct missing args, if any
    for (int i = n_args_on_callee_stack; i < n_args_callee - 1; i++)
      PUSH_OV ();

    int n_args_in_varargin = n_args_on_stack - n_args_callee + 1; // "Surplus" args
    int n_cells_left = cs_args.length () - cs_args_idx; // Amount of leftover cell ellements that need to go into varargin

    int idx_cell = 0;
    if (n_args_in_varargin > 0 || n_cells_left) // Anything to put in the varargin cell?
      {
        // TODO: Preallocate whole cell
        Cell cell(n_cells_left ? 1 : 0, n_cells_left);

        // Put the leftover objects from the cs-list expansion
        // in the varargin cell, if any
        for (int i = 0; i < n_cells_left; i++)
          cell (0, idx_cell++) = cs_args (cs_args_idx + i);

        // We need to expand cs-lists here too ...
        for (int i = 0; i < n_args_in_varargin; i++)
          {
            // int col = n_args_in_varargin - 1 - i;
            octave_value &arg = first_arg[ii + i].ov;

            if (arg.is_cs_list ())
              {
                octave_value_list cs_args_i = arg.list_value ();
                for (int j = 0; j < cs_args_i.length (); j++)
                  {
                    if (cell.numel () <= idx_cell)
                      cell.resize (dim_vector {1, idx_cell + 1});
                    cell (0, idx_cell++) = cs_args_i (j);
                  }
              }
            else
              {
                if (cell.numel () <= idx_cell)
                  cell.resize (dim_vector {1, idx_cell + 1});
                cell (0, idx_cell++) = std::move (arg);
              }

            arg.~octave_value ();
          }

        // Push varargin to the stack
        PUSH_OV (cell);
      }
    else
      PUSH_OV (Cell (0,0)); // Empty cell into varargin's slot

    // Construct locals
    int n_locals_to_ctor =
      n_locals_callee - n_args_callee - n_returns_callee;

    CHECK_STACK (n_locals_to_ctor);
    for (int i = 0; i < n_locals_to_ctor; i++)
      PUSH_OV ();

    int nargin = n_args_on_callee_stack + idx_cell; // n_args_callee count includes varargin

try
  {
    m_tw->push_stack_frame(*this, usr_fcn, nargout, n_args_on_callee_stack);
  }
CATCH_STACKPUSH_EXECUTION_EXCEPTION /* Sets m_could_not_push_frame to true */
CATCH_STACKPUSH_BAD_ALLOC

  m_tw->set_nargin (nargin);

if (OCTAVE_UNLIKELY (m_output_ignore_data))
  {
    /* Called fn needs to know about ignored outputs .e.g. [~, a] = foo() */
m_output_ignore_data->push_frame (*this);
  }

    /* N_RETURNS is negative for varargout */
    int n_returns = N_RETURNS () - 1; /* %nargout in N_RETURNS */
    if (n_returns >= 0 && nargout > n_returns)
      {
        (*sp++).pee = new execution_exception {"error","","function called with too many outputs"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        goto unwind;
      }

    // Now dispatch to first instruction in the
    // called function
  }
  DISPATCH ();

// Not an opcode. Some opcodes jump here to handle nested handle function calls
make_nested_handle_call:
{
  // Restore values from the stack
  int slot = (*--sp).i;
  int caller_nvalback = (*--sp).i;
  int nargout = (*--sp).i;
  int n_args_on_stack = (*--sp).i;

  octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

  octave_function *fcn;
  try
    {
      stack_element *first_arg = &sp[-n_args_on_stack];
      stack_element *end_arg = &sp[0];
      fcn = ov.get_cached_fcn (first_arg, end_arg);
    }
  CATCH_EXECUTION_EXCEPTION

  if (! fcn)
    {
      (*sp++).ps = new std::string {name_data[slot]};
      (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
      goto unwind;
    }

  if (fcn->is_compiled ())
    {
      octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);

      // The code bellow is like MAKE_BYTECODE_CALL, but with support for setting an access frame from the handle
      if (sp + stack_min_for_new_call >= m_stack + stack_size)
        {
          (*sp++).pee = new execution_exception {"error","","VM is running out of stack space"};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }
      /* We are now going to call another function */
      /* compiled to bytecode */

      m_tw->set_active_bytecode_ip (ip - code);
      stack_element *first_arg = sp - n_args_on_stack;

      /* Push address to first arg (or would one would have been */
      /* if there are no args), so we can restore the sp at return */
      (*sp++).pse = first_arg;

      /* Push unwind data */
      (*sp++).pud = unwind_data;

      /* Push code */
      (*sp++).puc = code;

      /* Push data */
      (*sp++).pov = data;

      /* Push id names */
      (*sp++).ps = name_data;

      /* Push bsp */
      (*sp++).pse = bsp;

      /* Push the instruction pointer */
      (*sp++).puc = ip;

      /* The amount of return values the caller actually wants. Not necesserely the */
      /* same as the amount of return values the caller wants the callee to produce. */
      /* (last on caller stack) */
      (*sp++).u = caller_nvalback;

      /* set callee bsp */
      m_sp = bsp = sp;

      /* Push nargout (first on callee stack) */
      (*sp++).u = nargout;

      /* Set the new data, code etc */
      bytecode &bc = usr_fcn->get_bytecode ();
      if (OCTAVE_UNLIKELY (m_profiler_enabled))
        {
          auto p = vm::m_vm_profiler;
          if (p)
            {
              std::string caller_name = data[2].string_value (); /* profiler_name () querried at compile time */
              p->enter_fn (caller_name, bc);
            }
        }
      m_data = data = bc.m_data.data ();
      m_code = code = bc.m_code.data ();
      m_name_data = name_data = bc.m_ids.data ();
      m_unwind_data = unwind_data = &bc.m_unwind_data;


      /* Set the ip to 0 */
      ip = code;
      int n_returns_callee = static_cast<signed char> (*ip++); /* Negative for varargout */
      if (OCTAVE_UNLIKELY (n_returns_callee < 0))
        {
          if (n_returns_callee == -128) /* Anonymous function */
            n_returns_callee = 1;
          else
            n_returns_callee = -n_returns_callee;
        }
      int n_args_callee = static_cast<signed char> (*ip++); /* Negative for varargin */
      int n_locals_callee = POP_CODE_USHORT ();

      if (n_args_callee < 0)
      {
        sp[0].pv = static_cast<void*> (usr_fcn);
        goto varargin_call;
      }

      /* Construct return values - note nargout */
      /* is allready pushed as a uint64 */
      for (int ii = 1; ii < n_returns_callee; ii++)
        PUSH_OV ();

      int n_args_on_callee_stack = 0;
      bool all_too_many_args = false;
      /* Move the args to the new stack */
      for (int ii = 0; ii < n_args_on_stack; ii++)
        {
          octave_value &arg = first_arg[ii].ov;

          if (arg.is_cs_list ())
            {
              octave_value_list args = arg.list_value ();
              octave_idx_type n_el = args.length ();
              if (n_el + n_args_on_callee_stack > 512)
                {
                  all_too_many_args = true;
                }
              else
                {
                  for (int j = 0; j < n_el; j++)
                    {
                      PUSH_OV (args (j));
                      n_args_on_callee_stack++;
                    }
                }
            }
          else
            {
              PUSH_OV (std::move (arg));
              n_args_on_callee_stack++;
            }
          /* Destroy the args */
          arg.~octave_value ();
        }
      /* Construct missing args */
      for (int ii = n_args_on_callee_stack; ii < n_args_callee; ii++)
        PUSH_OV ();

      /* Construct locals */
      int n_locals_to_ctor =
        n_locals_callee - n_args_callee - n_returns_callee;
      for (int ii = 0; ii < n_locals_to_ctor; ii++)
        PUSH_OV ();

      try
        {
          octave_fcn_handle *h = ov.fcn_handle_value();
          CHECK_PANIC (h);
          CHECK_PANIC (h->is_nested () || h->is_anonymous ());
          auto closure_frame = h->get_closure_frame ();

          m_tw->push_stack_frame(*this, usr_fcn, nargout, n_args_on_callee_stack, closure_frame);
        }
      CATCH_STACKPUSH_EXECUTION_EXCEPTION /* Sets m_could_not_push_frame to true */
      CATCH_STACKPUSH_BAD_ALLOC

      if (OCTAVE_UNLIKELY (m_output_ignore_data))
        {
          /* Called fn needs to know about ignored outputs .e.g. [~, a] = foo() */
          m_output_ignore_data->push_frame (*this);
        }

      /* "auto var" in the frame object. This is needed if nargout() etc are called */
      set_nargout (nargout);

      if (all_too_many_args)
        {
          std::string fn_name = unwind_data->m_name;
          (*sp++).pee = new execution_exception {"error", "Octave:invalid-fun-call",
                                                fn_name + ": function called with over 512 inputs."
                                                " Consider using varargin."};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }
      if (n_args_on_callee_stack > n_args_callee)
        {
          std::string fn_name = unwind_data->m_name;
          (*sp++).pee = new execution_exception {"error", "Octave:invalid-fun-call",
                                                fn_name + ": function called with too many inputs"};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }
      /* N_RETURNS is negative for varargout */
      int n_returns = N_RETURNS () - 1; /* %nargout in N_RETURNS */
      if (n_returns >= 0 && nargout > n_returns)
        {
          std::string fn_name = unwind_data->m_name;
          (*sp++).pee = new execution_exception {"error", "Octave:invalid-fun-call",
                                                fn_name + ": function called with too many outputs"};
          (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
          goto unwind;
        }

      // Now dispatch to first instruction in the
      // called function
    }
  else
    {
      // Make an ovl with the args
      octave_value_list ovl;
      // The operands are on the top of the stack
      POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

      try
        {
          m_tw->set_active_bytecode_ip (ip - code);
          octave_value_list ret = ov.simple_subsref ('(', ovl, nargout);
          ovl.clear ();

          STACK_DESTROY (1);
          EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
        }
      CATCH_INTERRUPT_EXCEPTION
      CATCH_INDEX_EXCEPTION_WITH_MAYBE_NAME(slot != 0)
      CATCH_EXECUTION_EXCEPTION
      CATCH_BAD_ALLOC
      CATCH_EXIT_EXCEPTION
    }
}
DISPATCH ();

unwind:
  {
    ip--; // Rewind ip to after the opcode (i.e. arg0's position in the code)
    // Push VM state
    m_sp = sp;
    m_bsp = bsp;
    m_rsp = rsp;
    m_code = code;
    m_data = data;
    m_name_data = name_data;
    m_ip = ip - code;
    m_unwind_data = unwind_data;

    m_echo_prior_op_was_cond = false; // Used by the echo functionality

    // Ther error_type is put on the stack before the jump to unwind.
    error_type et = static_cast<error_type> (m_sp[-1].i);
    m_sp--;

    // Save current exception to the error system in handle_error ()
    error_data errdat = handle_error (et);

    // Run only unwind_protect code if the exception is the interrupt exception.
    // I.e. no 'throw ... catch' code.
    bool only_unwind_protect = et == error_type::INTERRUPT_EXC;

    while (1)
      {
        // Find unwind entry for current value of the instruction pointer, unless we are dealing
        // with a debug quit exception in which case no unwind entry is used.
        unwind_entry *entry = nullptr;
        if (et != error_type::DEBUG_QUIT)
          entry = find_unwind_entry_for_current_state (only_unwind_protect);

        unwind_entry_type type = unwind_entry_type::INVALID;
        if (entry)
          type = entry->m_unwind_entry_type;

        // We need to figure out what stack depth we want.
        // If we are unwinding in a try catch we need to save any
        // nesting switch or for loop stack objects on the stack.
        int target_stack_depth = N_LOCALS();
        if (entry)
          {
            target_stack_depth += entry->m_stack_depth;
          }

        // Unwind the stack down to the locals
        //
        // If we got here from return op code we might allready have
        // destroyed the locals when an error triggered.
        while (m_sp - m_bsp > target_stack_depth)
          {
            // If the stack depth matches a for loop we need to
            // pop some native ints.
            //
            // TODO: Wasteful search for forloop each iteration
            int current_stack_depth = m_sp - m_bsp - N_LOCALS ();
            int stack_depth_for_forloop =
              find_unwind_entry_for_forloop (current_stack_depth);

            if (stack_depth_for_forloop != -1 &&
                current_stack_depth == stack_depth_for_forloop + 3)
              {
                m_sp -= 2; // Pop two ints
                (*--m_sp).ov.~octave_value (); // Pop ov
              }
            else
              (*--m_sp).ov.~octave_value ();
          }

        if (type == unwind_entry_type::UNWIND_PROTECT ||
            type == unwind_entry_type::TRY_CATCH)
          {
            // Need to set some stuff for last_error etc and make the
            // interpreter happy by reseting stuff
            error_system& es = m_tw->get_interpreter().get_error_system ();

            octave_scalar_map err_map;

            err_map.assign ("message", es.last_error_message ());
            err_map.assign ("identifier", es.last_error_id ());
            err_map.assign ("stack", es.last_error_stack ());

            m_tw->get_interpreter().recover_from_exception ();

            // Set stack pointer and ip and dispatch
            m_ip = entry->m_ip_target;
            code = m_code;
            ip = m_code + m_ip;
            sp = m_sp;

            // Push the error object that is either just poped right
            // away by a POP instruction or assigned to the catch
            // clause identifier.
            PUSH_OV (err_map);

            if (et == error_type::INTERRUPT_EXC)
              m_unwinding_interrupt = true;

            goto bail_unwind;
          }

        if (!m_could_not_push_frame)
          {
            auto sf = m_tw->get_current_stack_frame ();
            if (sf->is_user_script_frame ())
              sf->vm_exit_script ();
            sf->vm_unwinds ();
          }

        // Destroy locals down to nargout
        while (m_sp != m_bsp + 1)
          {
            (*--m_sp).ov.~octave_value ();
          }

        m_sp--; // nargout

        if (m_sp == m_rsp)
          break; // Got down to start of root stack frame

        if (OCTAVE_UNLIKELY (m_profiler_enabled))
          {
            auto p = vm::m_vm_profiler;
            if (p)
              {
                std::string fn_name = data[2].string_value (); // profiler_name () querried at compile time
                p->exit_fn (fn_name);
              }
          }

        // Skipp caller_nvalback
        m_sp--;

        // Restore ip
        ip = (*--m_sp).puc;

        // Restore bsp
        bsp = m_bsp = (*--m_sp).pse;

        // Restore id names
        name_data = m_name_data = (*--m_sp).ps;

        // Restore data
        data = m_data = (*--m_sp).pov;

        // Restore code
        code = m_code = (*--m_sp).puc;
        m_ip = ip - m_code;

        // Restore unwind data
        unwind_data = m_unwind_data = (*--m_sp).pud;

        // Restore the stack pointer
        sp = m_sp = m_sp[-1].pse;

        // Pop dynamic stackframe (unless it was never pushed)
        if (!m_could_not_push_frame)
          m_tw->pop_stack_frame ();
        else
          m_could_not_push_frame = false;

        // If we are messing with the interpreters lvalue_list due to some
        // ~ we need to restore stuff.
        if (m_output_ignore_data)
          {
            m_output_ignore_data->pop_frame (*this);
            output_ignore_data::maybe_delete_ignore_data (*this, 0);
          }
      }

    if (m_output_ignore_data)
      {
        CHECK_PANIC (m_output_ignore_data->m_external_root_ignorer);
        output_ignore_data::maybe_delete_ignore_data (*this, 1);
      }

    CHECK_PANIC (!m_output_ignore_data);

    CHECK_STACK (0);
    this->m_dbg_proper_return = true;

    m_tw->set_lvalue_list (m_original_lvalue_list);

    // Rethrow exceptions out of the VM
    if (et == error_type::INTERRUPT_EXC)
      throw interrupt_exception {};
    else if (et == error_type::DEBUG_QUIT)
      throw quit_debug_exception {errdat.m_debug_quit_all};
    else if (et == error_type::EXIT_EXCEPTION)
      throw exit_exception (errdat.m_exit_status, errdat.m_safe_to_return);
    else
      {
        error_system& es = m_tw->get_interpreter().get_error_system ();
        es.rethrow_error (es.last_error_id (), es.last_error_message (), es.last_error_stack ());
      }

  }
bail_unwind:
  DISPATCH ();

init_global:
  {
    // The next instruction tells whether we should init a global or persistent
    // variable.
    global_type type = static_cast<global_type> (arg0);

    // The next instruction is the local slot number for the global variable
    int slot = POP_CODE_USHORT();
    POP_CODE_USHORT(); // Not used TODO: Remove. Make this opcode use WIDE

    std::string& name = name_data[slot];

    octave_value &ov_slot = bsp[slot].ov;
    bool slot_already_live = ov_slot.is_defined ();

    bool is_marked_in_VM = ov_slot.is_ref (); // TODO: Can this be other refs?

    // The next instruction is whether the global declare has an
    // initialization value
    bool has_init_code = *ip++;

    // If the global was not allready created we need to assign a
    // empty double matrix to it.
    // If there already is a defined local in the slot we initialize
    // the global with the local
    // TODO: Should be a decrapation warning here for this
    octave_value ov_default;
    if (slot_already_live && !is_marked_in_VM)
      ov_default = std::move (ov_slot);
    else
      ov_default = Matrix ();

    if (!is_marked_in_VM)
      ov_slot = octave_value {};

    bool global_is_new_in_callstack = false;

    if (type == global_type::GLOBAL)
      {
        if (is_marked_in_VM && ov_slot.ref_rep ()->is_persistent_ref ())
          {
            (*sp++).pee = new execution_exception {"error", "",
              "can't make persistent variable '" + name + "' global"};
            (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
            goto unwind;
          }

        auto frame = m_tw->get_current_stack_frame ();
        auto sym = frame->insert_symbol (name);
        // Note: Install variable wont override global's value with nil ov from
        //       the "{}" argument.
        frame->install_variable (sym, {}, 1);

        octave_value &ov_gbl = m_tw->global_varref (name);
        global_is_new_in_callstack = ov_gbl.is_undefined ();

        // We assign the default before the init
        if (global_is_new_in_callstack)
          m_tw->global_assign (name, ov_default);

        if (!is_marked_in_VM)
          {
            ov_slot = octave_value {new octave_value_ref_global {name}};
          }

        // TODO: Assert global_is_new_in_callstack != global_is_marked_in_VM
        // but does not work until the dynamic stack is implemented.

        // CHECK (global_is_new_in_callstack != global_is_marked_in_VM);
      }
    else if (type == global_type::PERSISTENT)
      {
        if (is_marked_in_VM && ov_slot.ref_rep ()->is_global_ref ())
          {
            (*sp++).pee = new execution_exception {"error", "",
              "can't make global variable '" + name + "' persistent"};
            (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
            goto unwind;
          }

        auto frame = m_tw->get_current_stack_frame();

        symbol_record sym = frame->lookup_symbol(name);
        try
          {
            // Throws if global or formal parameter
            frame->make_persistent(sym);
          }
        CATCH_EXECUTION_EXCEPTION

        auto scope = frame->get_scope ();

        // TODO: Put the offset in the op-code instead?
        auto it = unwind_data->m_slot_to_persistent_slot.find (slot);
        CHECK (it != unwind_data->m_slot_to_persistent_slot.end ());
        int pers_offset = it->second;

        octave_value &ov_gbl = scope.persistent_varref (pers_offset);

        global_is_new_in_callstack = ov_gbl.is_undefined ();
        if (global_is_new_in_callstack)
          {
            ov_gbl = ov_default;
          }

        if (!is_marked_in_VM)
          {
            ov_slot = octave_value {new octave_value_ref_persistent {std::move (scope), pers_offset}};
          }
      }
    else
      ERR ("Wrong global type");

    // If there is init code, then there is also a offset to the first
    // instruction after the init code, to where we jump if the global is
    // alread live.
    int after;
    if (has_init_code)
      {
        unsigned char b0 = *ip++;
        unsigned char b1 = *ip++;
        after = USHORT_FROM_UCHARS (b0, b1);

        if (!global_is_new_in_callstack || slot_already_live)
          ip = code + after;
      }

    // Now dispatch to either next instruction if no init, init or after init
  }
  DISPATCH ();
assign_compound:
  {
    // The next instruction is the slot number
    int slot = arg0;
    // The next instruction is the type of compound operation
    octave_value::assign_op op =
      static_cast<octave_value::assign_op> (*ip++);

    octave_value &ov_rhs = TOP_OV ();
    octave_value &ov_lhs = bsp[slot].ov;

    if (!ov_lhs.is_defined ()) // TODO: Also checked in .assign() ...
      {
        (*sp++).pee = new execution_exception {"error", "",
          "in computed assignment A OP= X, A must be defined first"};
        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        goto unwind;
      }

    try
      {
        // TODO: assign makes some stupid empty list and slows everything down
        if (OCTAVE_LIKELY (!ov_lhs.is_ref ()))
          ov_lhs.assign (op, ov_rhs); // Move code into here?
        else
          {
            octave_value &glb_ref = ov_lhs.ref_rep ()->ref ();
            glb_ref.assign (op, ov_rhs);
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    STACK_DESTROY (1);
  }
  DISPATCH ();
jmp_ifdef:
  {
    octave_value &ov_1 = TOP_OV ();
    unsigned char b0 = arg0;
    unsigned char b1 = *ip++;

    int target = USHORT_FROM_UCHARS (b0, b1);

    if (ov_1.is_defined () && !ov_1.is_magic_colon ())
      ip = code + target;

    STACK_DESTROY (1);
  }
  DISPATCH ();
switch_cmp:
  {
    octave_value &ov_label = TOP_OV ();
    octave_value &ov_switch = SEC_OV ();
    unsigned char b0 = arg0;
    unsigned char b1 = *ip++;

    int target = USHORT_FROM_UCHARS (b0, b1);

    bool do_it;
    if (ov_label.is_undefined ())
      do_it = false;
    else if (!ov_label.iscell ())
      do_it = ov_switch.is_equal (ov_label);
    else
      {
        do_it = false;
        // Match all cell elements. Any will do
        Cell cell (ov_label.cell_value ());

        for (octave_idx_type i = 0; i < cell.rows (); i++)
          {
            for (octave_idx_type j = 0; j < cell.columns (); j++)
              {
                do_it = ov_switch.is_equal (cell(i,j));

                if (do_it)
                  break;
              }
          }
      }

    STACK_DESTROY (2);

    if (!do_it)
      ip = code + target;
  }
  DISPATCH ();

braindead_precond:
  {
    octave_value &ov = TOP_OV();

    bool do_braindead = false;
    if (ov.ndims () == 2 && ov.rows () == 1 && ov.columns () == 1)
      do_braindead = true;

    STACK_DESTROY (1);

    if (do_braindead)
      PUSH_OV (ov_true);
    else
      PUSH_OV (ov_false);
  }
  DISPATCH_1BYTEOP ();

braindead_warning:
  {
    // A slot stores whether we allready printed this warning for a particular
    // place where there could be a braindead short circuit
    int slot = arg0;
    // The next codepoint is the type of warning
    int type = *ip++; // asci '|' or '&'

    octave_value& ov_warning = bsp[slot].ov;

    if (ov_warning.is_nil ())
      {
        ov_warning = ov_true; // Don't print the warning next time
        m_tw->set_active_bytecode_ip (ip - code); // The warning needs to be able to get line numbers.

        // It is possible to specify that certain warning should be an error, so we need a try here.
        try
          {
            warning_with_id ("Octave:possible-matlab-short-circuit-operator",
                            "Matlab-style short-circuit operation performed for operator %c",
                            type);
          }
        CATCH_EXECUTION_EXCEPTION
      }
  }
  DISPATCH ();
force_assign:
  {
    // The next instruction is the slot number
    int slot = arg0;

    octave_value &ov_rhs = TOP_OV ();
    octave_value &ov_lhs = bsp[slot].ov;

    ov_lhs.maybe_call_dtor ();

    if (ov_rhs.vm_need_storable_call ())
      ov_rhs.make_storable_value (); // Some types have lazy copy

    if (OCTAVE_LIKELY (!ov_lhs.is_ref ()))
      ov_lhs = std::move (ov_rhs); // Note move
    else
      ov_lhs.ref_rep ()->set_value (std::move (ov_rhs));

    STACK_DESTROY (1);
  }
  DISPATCH();
push_nil:
  {
    PUSH_OV(octave_value{});
  }
  DISPATCH_1BYTEOP();
throw_iferrorobj:
  {
    octave_value& ov_top = TOP_OV ();

    if (ov_top.is_defined ())
      {
        // This "error object" is created by the unwind: code
        // and e.g. not from a user's error
        octave_scalar_map map = ov_top.scalar_map_value ();

        bool is_err_obj = map.isfield("message") &&
                          map.isfield ("identifier") &&
                          map.isfield ("stack");

        if (!is_err_obj)
          PANIC ("Strange error object on stack");

        octave_value msg = map.getfield ("message");
        octave_value id = map.getfield ("identifier");

        STACK_DESTROY (1);

        std::string s_msg  = msg.string_value ();
        std::string s_id = id.string_value ();

        octave_map err_stack = map.contents ("stack").xmap_value ("ERR.STACK must be a struct");

        // Are we unwinding an interrupt exception?
        if (m_unwinding_interrupt)
          {
            (*sp++).i = static_cast<int> (error_type::INTERRUPT_EXC);
            goto unwind;
          }

        // On a rethrow, the C++ exception is always base class execution_exception.
        // We use rethrow_error() to recreate a stack info object from the octave_map
        // in an easy way.
        try
        {
          error_system& es = m_tw->get_interpreter().get_error_system ();
          es.rethrow_error (s_id, s_msg, err_stack);
        }
        catch(execution_exception& e)
        {
          (*sp++).pee =  new execution_exception {e};
        }

        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
        goto unwind;
      }
    else
      STACK_DESTROY (1);
  }
  DISPATCH_1BYTEOP();

index_struct_call:
  {
    // This opcode is a setup for a chain of opcodes that each does a part
    // in a chained subsref.
    //
    // It is done like this since there can be bytecode function calls in the middle
    // of the chain and the call can only return to an opcode, not some arbitrary point in
    // the C++ code.

    int nargout = arg0;
    int slot = POP_CODE_USHORT ();
    int n_args_on_stack = POP_CODE ();
    char type = POP_CODE ();

    // The object being indexed is on the stack under the arguments
    octave_value &ov = sp[-1 - n_args_on_stack].ov;

    // If there is a slot specified, we need to check if we need to call
    // its octave_value (e.g. a handle) or a function corrensponding to its name.
    if (slot)
      {
        if (ov.is_nil ())
          {
            // The slot the object being indexed was pushed from
            octave_value &slot_ov = bsp[slot].ov;

            // Put a function cache object in the slot and in the local ov
            ov = octave_value (new octave_fcn_cache (name_data[slot]));
            if (slot_ov.is_ref())
              slot_ov.ref_rep ()->set_value (ov);
            else
              slot_ov = ov;
          }

        // Should we call the object?
        if (ov.vm_dispatch_call ()
            == octave_base_value::vm_call_dispatch_type::OCT_CALL)
          {
            CHECK_PANIC (ov.has_function_cache ());

            octave_function *fcn;

            octave_value_list ovl;
            // The operands are on the top of the stack
            POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

            try
              {
                if (type == '(')
                  {
                    fcn = ov.get_cached_fcn (ovl);
                  }
                else // { or .
                  {
                    fcn = ov.get_cached_fcn (static_cast<octave_value*> (nullptr), static_cast<octave_value*> (nullptr));
                  }
              }
            CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

            if (! fcn)
              {
                (*sp++).ps = new std::string {name_data[slot]};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }

            try
              {
                // TODO: Bytecode call

                octave_value_list retval;
                m_tw->set_active_bytecode_ip (ip - code);
                if (type == '(')
                  {
                     // Skip the following subsref. Need to check for nargout extension.
                    if (*ip == static_cast<unsigned char> (INSTR::EXT_NARGOUT))
                      ip += 7; // Skip EXT_NARGOUT + STRUCT_INDEX_SUBCALL
                    else
                      ip += 6; // Skip STRUCT_INDEX_SUBCALL
                    retval = fcn->call (*m_tw, nargout, ovl);
                  }
                else
                  {
                    retval = fcn->call (*m_tw, nargout, {});
                  }

                STACK_DESTROY (1); // Destroy the ov being indexed
                PUSH_OV (retval.first_or_nil_ov ()); // Push the next ov to be indexed
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION
          }
      }

  // The next instruction is a INDEX_STRUCT_SUBCALL
  // One for each part of the chain
  }
  DISPATCH (); // TODO: Make hardcoded goto to index_struct_subcall:

index_struct_n:
  {
    int nargout = arg0;

    int slot = POP_CODE_USHORT (); // Needed if we need a function lookup
    int slot_for_field = POP_CODE_USHORT ();

    octave_value &ov = TOP_OV ();

    std::string field_name = name_data [slot_for_field];

    octave_value ov_field_name {field_name};

    octave_value_list retval;

    // TODO: Should be a "simple_subsref for "{" and "."
    octave_value_list ovl_idx;
    ovl_idx.append (ov_field_name);

    std::list<octave_value_list> idx;
    idx.push_back (ovl_idx);

    try
      {
        m_tw->set_active_bytecode_ip (ip - code);
        retval = ov.subsref(".", idx, nargout);

        // TODO: Kludge for e.g. "m = containsers.Map;" which returns a function.
        //       Should preferably be done by .subsref?
        octave_value val = (retval.length () ? retval(0) : octave_value ());
        if (val.is_function ())
          {
            octave_function *fcn = val.function_value (true);

            if (fcn)
              {
                retval = fcn->call (*m_tw, nargout, {});
              }
          }

        idx.clear ();
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    STACK_DESTROY (1);
    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
  }
  DISPATCH ();

subasgn_struct:
  {
    int slot = arg0;
    int field_slot = POP_CODE_USHORT ();

    // The top of the stack is the rhs value
    octave_value &rhs = TOP_OV ();

    // The ov to subassign to
    octave_value &ov = bsp[slot].ov;

    // TODO: Room for performance improvement here maybe
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      ov.make_unique ();
    else
      ov.ref_rep ()->ref ().make_unique ();

    // TODO: Uggly containers
    std::list<octave_value_list> idx;
    octave_value_list ovl;

    std::string field_name = name_data[field_slot];

    octave_value ov_field_name {field_name};

    ovl.append (ov_field_name);

    idx.push_back (ovl);

    // E.g. scalars do not update them self inplace
    // but create a new octave_value, so we need to
    // copy the return value to the slot.
    try
      {
        ov = ov.subsasgn (".", idx, rhs);
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    STACK_DESTROY (1);
  }
  DISPATCH ();

subasgn_cell_id:
  {
    // The args to the subassign are on the operand stack
    int slot = arg0;
    int nargs = *ip++;

    // The top of the stack is the rhs value
    octave_value &rhs = TOP_OV ();
    // First argument
    stack_element *parg = sp - 1 - nargs;

    // Move the args to an ovl
    // TODO: Should actually be a move
    octave_value_list args;
    for (int i = 0; i < nargs; i++)
    {
      octave_value &arg = parg[i].ov;
      // We need to expand cs-lists
      if (arg.is_cs_list ())
        args.append (arg.list_value ());
      else
        args.append (arg);
    }

    // The ov to subassign to
    octave_value &ov = bsp[slot].ov;
    // TODO: Room for performance improvement here maybe
    if (OCTAVE_LIKELY (!ov.is_ref ()))
      ov.make_unique ();
    else
      ov.ref_rep ()->ref ().make_unique ();

    // TODO: Uggly containers
    std::list<octave_value_list> idx;
    idx.push_back (args);

    try
      {
        // E.g. scalars do not update them self inplace
        // but create a new octave_value, so we need to
        // copy the return value to the slot.
        ov = ov.subsasgn("{", idx, rhs);
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION_WITH_NAME
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    // Destroy the args on the operand stack aswell as rhs
    STACK_DESTROY (nargs + 1);
  }
  DISPATCH ();

subassign_obj:
  {
    // The args to the subassign are on the operand stack
    int nargs = arg0;
    char type = *ip++;

    // First argument
    stack_element *parg = sp - nargs;
    // lhs is under the args -- the target for the subassign
    octave_value &lhs = (sp - nargs - 1)->ov;
    lhs.make_unique (); // TODO: Room for performance improvement here maybe
    // rhs is under the lhs
    octave_value &rhs = (sp - nargs - 2)->ov; // lhs is written to this stack position

    // Move the args to an ovl
    // TODO: Should actually be a move
    octave_value_list args;
    for (int i = 0; i < nargs; i++)
    {
      octave_value &arg = parg[i].ov;
      // We need to expand cs-lists
      if (arg.is_cs_list ())
        args.append (arg.list_value ());
      else
        args.append (arg);
    }

    // TODO: Uggly containers
    std::list<octave_value_list> idx;
    idx.push_back (args);

    try
      {
        // E.g. scalars do not update them self inplace
        // but create a new octave_value, so we need to
        // copy the return value to the slot.
        lhs = lhs.subsasgn(std::string {type}, idx, rhs);
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION

    // We want lhs on the top of the stack after dropping all
    // the args to SUBASSIGN_OBJ, so we move it to where rhs is
    rhs = std::move (lhs);

    // Destroy the args on the operand stack aswell as the
    // stack position that we moved lhs out of.
    STACK_DESTROY (nargs + 1);

    // lhs is on the top of the stack now
  }
  DISPATCH ();

index_obj:
  {
    int nargout = arg0;
    int has_slot = *ip++;
    int slot = POP_CODE_USHORT ();
    int n_args_on_stack = *ip++;
    char type = *ip++;

    // The object to index is before the args on the stack
    octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

    switch (ov.vm_dispatch_call ())
      {
        case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
          PANIC ("Invalid dispatch");
        case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
          {
            // TODO: subsref should take ovl instead and be chained,
            // or something smarter
            std::list<octave_value_list> idx; // TODO: mallocs!

            // Make an ovl with the args
            octave_value_list ovl;
            // The operands are on the top of the stack
            POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

            idx.push_back(ovl);

            octave_value_list retval;

            try
              {
                m_tw->set_active_bytecode_ip (ip - code);
                retval = ov.subsref(std::string {type}, idx, nargout);
                idx.clear ();
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION_WITH_MAYBE_NAME (has_slot)
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION

            bool is_fcn = (retval.length () ?
                            retval(0).is_function() : false);

            // "FIXME: when can the following happen?  In what case does indexing
            //  result in a value that is a function?  Classdef method calls?
            //  Something else?"

            if (OCTAVE_LIKELY (!is_fcn))
              {
                idx.clear ();
                // TODO: Necessary? I guess it might trigger dtors
                // or something?
                ov = octave_value ();
              }
            else
              {
                octave_value val = retval(0);
                octave_function *fcn = val.function_value (true);

                if (fcn)
                  {
                    octave_value_list final_args;

                    if (! idx.empty ())
                      final_args = idx.front ();

                    try
                      {
                        m_tw->set_active_bytecode_ip (ip - code);
                        retval = fcn->call (*m_tw, nargout, final_args);
                      }
                    CATCH_INTERRUPT_EXCEPTION
                    CATCH_INDEX_EXCEPTION_WITH_MAYBE_NAME (has_slot)
                    CATCH_EXECUTION_EXCEPTION
                    CATCH_BAD_ALLOC
                    CATCH_EXIT_EXCEPTION
                  }

                idx.clear ();
                ov = octave_value ();
                val = octave_value ();
              }

            // Destroy the indexed variable on the stack
            STACK_DESTROY (1);
            EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
          }
        break;

        case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
          {
            // If the first object is not an identifier we can't look it up for
            // a function call.
            if (!has_slot)
              {
                (*sp++).ps = new std::string {"temporary object"};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }

            if (! ov.is_nil ())
              {
                TODO ("Not nil object for fcn cache replacement");
              }

            // It is probably a function call.
            // Put a function cache object in the slot and in the local ov
            // and jump into the if clause above to search for some function
            // to call.
            ov = octave_value (new octave_fcn_cache (name_data[slot]));
            if (bsp[slot].ov.is_ref ())
              bsp[slot].ov.ref_rep ()->set_value (ov);
            else
              bsp[slot].ov = ov;
          }
        // Fallthrough
        case octave_base_value::vm_call_dispatch_type::OCT_CALL:
        case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
        case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
          {
            octave_function *fcn;
            try
              {
                stack_element *first_arg = &sp[-n_args_on_stack];
                stack_element *end_arg = &sp[0];
                fcn = ov.get_cached_fcn (first_arg, end_arg);
              }
            CATCH_EXECUTION_EXCEPTION

            if (! fcn)
              {
                if (has_slot)
                  (*sp++).ps = new std::string {name_data[slot]};
                else
                  (*sp++).ps = new std::string {"temporary object"};
                (*sp++).i = static_cast<int> (error_type::ID_UNDEFINED);
                goto unwind;
              }

            if (fcn->is_compiled ())
              {
                octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);
                // Alot of code in this define
                int caller_nvalback = nargout; // Caller wants as many values as it wants the callee to produce
                MAKE_BYTECODE_CALL

                // Now dispatch to first instruction in the
                // called function
              }
            else
              {
                // Make an ovl with the args
                octave_value_list ovl;
                // The operands are on the top of the stack
                POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                try
                  {
                    m_tw->set_active_bytecode_ip (ip - code);
                    octave_value_list ret = fcn->call (*m_tw, nargout, ovl);

                    STACK_DESTROY (1);
                    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                  }
                CATCH_INTERRUPT_EXCEPTION
                CATCH_INDEX_EXCEPTION
                CATCH_EXECUTION_EXCEPTION
                CATCH_BAD_ALLOC
                CATCH_EXIT_EXCEPTION

              }
          }
      }

  }
  DISPATCH ();
load_far_cst:
  {
    ip--;
    int offset = POP_CODE_INT ();

    // Copy construct it into the top of the stack
    new (sp++) octave_value (data [offset]);

    DISPATCH();
  }

anon_maybe_set_ignore_output:
  {
    if (m_output_ignore_data)
      {
        // We want to propagate the caller's ignore matrix.
        octave_value current_ignore_matrix = m_tw->get_auto_fcn_var (stack_frame::auto_var_type::IGNORED);
        m_output_ignore_data->set_ignore_anon (*this, current_ignore_matrix);
      }
  }
  DISPATCH_1BYTEOP ();

set_ignore_outputs:
  {
    if (!m_output_ignore_data)
      {
        m_output_ignore_data = new output_ignore_data;
      }

    int n_ignored = arg0;
    int n_total = POP_CODE ();

    Matrix M;
    M.resize (1, n_ignored);

    std::set<int> set_ignored;

    for (int i = 0; i < n_ignored; i++)
      {
        int ignore_idx = POP_CODE ();
        M (i) = ignore_idx;
        set_ignored.insert (ignore_idx);
      }

    octave_value ignore_matrix {M};

    // For calls into m-functions etc
    auto *new_lvalue_list = new std::list<octave_lvalue> {};

    for (int i = 0; i < n_total; i++)
      {
        octave_lvalue lval ({}, m_tw->get_current_stack_frame ());
        if (set_ignored.find (i + 1) != set_ignored.end ())
          lval.mark_black_hole ();
        new_lvalue_list->push_back (lval);
      }

    m_output_ignore_data->set_ignore (*this, ignore_matrix, new_lvalue_list);
  }
  DISPATCH();

clear_ignore_outputs:
  {
    if (m_output_ignore_data)
      m_output_ignore_data->clear_ignore (*this);

    output_ignore_data::maybe_delete_ignore_data (*this, 1);

    // Clear any value written to the %~X slot(s)
    int n_slots = arg0;
    for (int i = 0; i < n_slots; i++)
      {
        int slot = POP_CODE_USHORT ();

        octave_value &ov = bsp[slot].ov;

        if (ov.get_count () == 1)
          ov.call_object_destructor ();

        ov = octave_value{};
      }
  }
  DISPATCH();

subassign_chained:
  {
    int slot = arg0;
    octave_value::assign_op op = static_cast<octave_value::assign_op> (*ip++);
    int n_chained = POP_CODE ();
    std::vector<int> v_n_args;
    std::string type (n_chained, 0);

    for (int i = 0; i < n_chained; i++)
      {
        v_n_args.push_back (POP_CODE ());
        type [i] = POP_CODE ();
      }

    std::list<octave_value_list> idx;
    for (int i = 0; i < n_chained; i++)
      {
        octave_value_list ovl;
        // foo (a1, a2).bar (a3, a4)
        // are:
        // TOP a4, a3, a2, a1
        // on the stack now.
        int n_args = v_n_args [n_chained - i - 1];
        for (int j = 0; j < n_args; j++)
          {
            octave_value &arg = TOP_OV ();
            if (arg.is_cs_list ())
              ovl.append (arg.list_value ().reverse ()); // Expand cs-list
            else
              ovl.append (std::move (arg));
            STACK_DESTROY (1);
          }
        ovl.reverse ();
        idx.push_back (ovl);
      }

    idx.reverse ();

    octave_value lhs = std::move (TOP_OV ());
    STACK_DESTROY (1);
    octave_value rhs = std::move (TOP_OV ());
    STACK_DESTROY (1);

    try
      {
        if (type.size () && type.back () != '(' && lhs_assign_numel (lhs, type, idx) != 1)
          err_invalid_structure_assignment ();

        if (slot)
          {
            octave_value &lhs_slot = bsp[slot].ov;

            // We don't need to he lhs value put on the stack since we are working on a slot.
            // Clear it to make assigns not need a new copy.
            lhs = octave_value {};

            if (OCTAVE_UNLIKELY (lhs_slot.is_ref ()))
              {
                octave_value &ov_ref = lhs_slot.ref_rep ()->ref ();
                ov_ref.make_unique ();
                ov_ref.assign (op, type, idx, rhs);
              }
            else
              lhs_slot.assign (op, type, idx, rhs);

            // Push a dummy octave_value. Always poped by a POP opcode.
            PUSH_OV (octave_value {});
          }
        else
          {
            lhs.assign (op, type, idx, rhs);
            // The value is pushed and used for further chaining.
            PUSH_OV (lhs);
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH ();

set_slot_to_stack_depth:
  {
    int slot = arg0;
    int stack_depth = sp - bsp;
    bsp[slot].ov = octave_value {stack_depth};
  }
  DISPATCH ();
dupn:
  {
    int offset = arg0;
    int n = POP_CODE ();
    stack_element *first = sp - n - offset;
    for (int i = 0; i < n; i++)
      PUSH_OV (first[i].ov);
  }
  DISPATCH ();
load_cst_alt2:
  {
    int offset = arg0;

    // Copy construct it into the top of the stack
    new (sp++) octave_value (data [offset]);

    DISPATCH ();
  }
load_cst_alt3:
  {
    int offset = arg0;

    // Copy construct it into the top of the stack
    new (sp++) octave_value (data [offset]);

    DISPATCH ();
  }
load_cst_alt4:
  {
    int offset = arg0;

    // Copy construct it into the top of the stack
    new (sp++) octave_value (data [offset]);

    DISPATCH ();
  }
load_2_cst:
{
  // We are pushing two constants to the stack. E.g. for "3 * 2".
  // The next instruction is the offset in the data of the lhs.
  // rhs is right after.
  int offset = arg0;

  // Copy construct the two constants onto the top of the stack
  new (sp++) octave_value (data [offset]);     // lhs in a binop
  new (sp++) octave_value (data [offset + 1]); // rhs

  DISPATCH ();
}

ret_anon:
  {
    // We need to tell the bytecode frame we are unwinding so that it can save
    // variables on the VM stack if it is referenced from somewhere else.
    m_tw->get_current_stack_frame ()->vm_unwinds ();

    assert (N_RETURNS () == -128);

    int n_returns_callee = bsp[0].i; // Nargout on stack
    if (n_returns_callee == 0)
      n_returns_callee = 1;

    int n_locals_callee = N_LOCALS (); // Amount of arguments, purely local variables and %nargout

    int n_ret_on_stack = sp - bsp - n_locals_callee;

    // Assert that the stack pointer is back where it should be, i.e. that there are between
    // zero and nargout return values.
    assert (n_ret_on_stack >= 0 && n_ret_on_stack <= n_returns_callee);

    stack_element *first_ret = sp - n_ret_on_stack;

    // Destroy locals
    //
    // Note that we destroy from the bottom towards
    // the top of the stack to calls ctors in the same
    // order as the treewalker.

    stack_element *first_pure_local = bsp + 1;
    while (first_pure_local != first_ret)
      {
        (*first_pure_local++).ov.~octave_value ();
      }

    if (OCTAVE_UNLIKELY (m_profiler_enabled))
      {
        auto p = vm::m_vm_profiler;
        if (p)
          {
            std::string fn_name = data[2].string_value (); // profiler_name () querried at compile time
            p->exit_fn (fn_name);
          }
      }

    // If we have any active ~/"black hole", e.g. [~] = foo() in the stack
    // the m_output_ignore_data pointer is live. We need to pop and reset
    // lvalue lists for the tree walker.
    if (OCTAVE_UNLIKELY (m_output_ignore_data))
      {
        m_output_ignore_data->pop_frame (*this);
        output_ignore_data::maybe_delete_ignore_data (*this, 0);
      }

    // Are we at the root routine?
    if (bsp == rsp)
      {
        CHECK (!m_output_ignore_data); // Should not be active

        // Collect return values in octave_value_list.
        // Skip %nargout, the first value, which is an integer.
        // n_returns_callee includes %nargout, but root_nargout doesn't.
        octave_value_list ret;

        int j;
        // nargout 0 should still give one return value, if there is one
        int n_root_wanted = std::max (root_nargout, 1);
        for (j = 0; j < n_ret_on_stack && j < n_root_wanted; j++)
          {
            int idx = n_ret_on_stack - 1 - j;
            ret.append (std::move (first_ret[idx].ov));
            first_ret[idx].ov.~octave_value ();
          }
        // Destroy rest of return values, if any
        for (; j < n_ret_on_stack; j++)
          {
            int idx = n_ret_on_stack - j;
            first_ret[idx].ov.~octave_value ();
          }

        //Note: Stack frame object popped by caller
        CHECK_STACK (0);
        this->m_dbg_proper_return = true;

        m_tw->set_lvalue_list (m_original_lvalue_list);
        return ret;
      }

    // If the root stack pointer is not the same as the base pointer,
    // we are returning from a bytecode routine to another bytecode routine,
    // so we have to restore the caller stack frame and cleanup the callee's.
    //
    // Essentially do the same thing as in the call but in reverse order.

    // Point sp one past the last return value
    stack_element *caller_stack_end = bsp;
    sp = caller_stack_end; // sp points to one past caller stack

    // The amount of return values the caller wants, as stored last on the caller stack.
    // Note that this is not necessarily the same as nargout, the amount of return values the caller
    // want the callee to produce, stored first on callee stack.
    int caller_nval_back = (*--sp).u;

    // Restore ip
    ip = (*--sp).puc;

    // Restore bsp
    bsp = (*--sp).pse;

    // Restore id names
    name_data = (*--sp).ps;

    // Restore data
    data = (*--sp).pov;

    // Restore code
    code = (*--sp).puc;

    // Restore unwind data
    unwind_data = (*--sp).pud;

    // Restore the stack pointer. The stored address is the first arg
    // on the caller stack, or where it would have been if there are no args.
    // The args were moved to the callee stack and destroyed on the caller
    // stack in the call.
    sp = sp[-1].pse;

    // We now have the object that was called on the stack, destroy it
    STACK_DESTROY (1);

    // Move the callee's return values to the top of the stack of the caller.
    // Renaming variables to keep my sanity.
    int n_args_caller_expects = caller_nval_back;
    int n_args_callee_has = n_ret_on_stack; // Excludes %nargout
    int n_args_to_move = std::min (n_args_caller_expects, n_args_callee_has);
    int n_args_actually_moved = 0;

    // If no return values is requested but there exists return values,
    // we need to push one to be able to write it to ans.
    if (n_args_caller_expects == 0 && n_args_callee_has)
      {
        n_args_actually_moved++;
        PUSH_OV (std::move (first_ret[0].ov));
      }
    // If the callee aint returning anything, we need to push a
    // nil object, since the caller always anticipates atleast
    // one object, even for nargout == 0.
    else if (n_args_caller_expects == 0 && !n_args_callee_has)
      PUSH_OV();
    // If the stacks will overlap due to many returns, do copy via container
    else if (sp + n_args_caller_expects >= caller_stack_end)
      {
        // This pushes 'n_args_to_move' number of return values and 'n_args_caller_expects - n_args_to_move'
        // number of nils.
        copy_many_args_to_caller (sp, first_ret, n_args_to_move, n_args_caller_expects);
        n_args_actually_moved = n_args_caller_expects;
        sp += n_args_actually_moved;
      }
    // Move 'n_args_to_move' return value from callee to caller
    else
      {
        // If the caller wants '[a, b, ~]' and the callee has 'd e'
        // we need to push 'nil' 'd' 'e'
        for (int i = n_args_to_move; i < n_args_caller_expects; i++)
          PUSH_OV ();
        for (int i = 0; i < n_args_to_move; i++)
          {
            // Move into caller stack. Note that the order is reversed, such that
            // a b c on the callee stack becomes c b a on the caller stack.
            octave_value &arg = first_ret[i].ov;

            PUSH_OV (std::move (arg));
          }
        n_args_actually_moved = n_args_caller_expects;
      }

    // Destroy the unused return values on the callee stack
    for (int i = 0; i < n_args_callee_has; i++)
      {
        int idx = n_args_callee_has - 1 - i;
        first_ret[idx].ov.~octave_value (); // Destroy ov in callee
      }

    // Pop the current dynamic stack frame
    std::shared_ptr<stack_frame> fp = m_tw->pop_return_stack_frame ();
    // If the pointer is not shared, stash it in a cache which is used
    // to avoid having to allocate shared pointers each frame push.
    if (fp.unique () && m_frame_ptr_cache.size () < 8)
      {
        fp->vm_clear_for_cache ();
        m_frame_ptr_cache.push_back (std::move (fp));
      }

    // Continue execution back in the caller
  }
  DISPATCH ();

/* Check whether we should enter the debugger on the next ip */
{
  bool onebyte_op;
  if (0)
    debug_check:
    onebyte_op = false;
  else if (0)
    debug_check_1b:
    onebyte_op = true;

  {
    int tmp_ip = ip - code;
    if (onebyte_op)
      tmp_ip--;

    if (OCTAVE_UNLIKELY (m_trace_enabled))
      {
        PRINT_VM_STATE ("Trace: ");
      }

    // Handle the VM profiler
    if (OCTAVE_UNLIKELY (m_profiler_enabled))
      {
        int64_t t1 = vm_profiler::unow ();

        auto p = m_vm_profiler;
        if (!p) // Only happens as a race between m_profiler_enabled and m_vm_profiler
          goto debug_check_end;

        std::string fn_name = data[2].string_value (); // profiler_name () querried at compile time
        vm_profiler::vm_profiler_fn_stats &stat = p->m_map_fn_stats[fn_name];

        if (!stat.m_v_t.size ())
          {
            // The profiler got enabled after the current function was called.
            p->enter_fn (fn_name, "", unwind_data, name_data, code);
            stat.m_v_t.back () = -1;
            stat.m_v_ip.back () = ip - code; // We are not at function start, so set ip to proper value.
          }
        else if (stat.m_v_t.back () != -1)
          {
            int64_t t0 = stat.m_v_t.back ();
            int64_t dt = t1 - t0;

            stat.add_t (dt);
            p->add_t (dt);
          }
      }

    // Handle the echo functionality.
    if (m_tw->echo ())
      {
        int ip_offset = ip - code;
        // In the beginning of functions we need to push an echo state for the function.
        // push_echo_state () checks e.g. if the current function is supposed to be printed.
        // The check is querried with echo_state ().
        if (ip_offset == 4) // TODO: Make constexpr for first opcode offset
          {
            int type = m_unwind_data->m_is_script ? tree_evaluator::ECHO_SCRIPTS : tree_evaluator::ECHO_FUNCTIONS;
            m_tw->push_echo_state (type, m_unwind_data->m_file);
          }

        if (!m_tw->echo_state ())
          goto bail_echo;

        auto it = unwind_data->m_ip_to_tree.find (tmp_ip);
        if (it == unwind_data->m_ip_to_tree.end ())
          goto bail_echo;

        tree *t = it->second;
        if (!t)
          goto bail_echo;

        int line = t->line ();
        if (line < 0)
          line = 1;

        // We don't want to echo the condition checks in for loops, but
        // reset the "last echoed" line to echo the next line properly.
        switch (static_cast<INSTR> (*ip))
          {
            case INSTR::FOR_COND:
            case INSTR::FOR_COMPLEX_COND:
              m_echo_prior_op_was_cond = true;
              goto bail_echo;
            default:
              break;
          }

        if (m_echo_prior_op_was_cond)
          {
            m_echo_prior_op_was_cond = false;
            m_tw->set_echo_file_pos (line);
          }

        try
          {
            m_tw->echo_code (line);
          }
        catch (execution_exception &)
          {
            // echo_code() might throw if there is no file info
            // attached to the executing function.
            // Just ignore it.
            // TODO: Might be a bug? Does this apply to the tree_evaluator?
            //       Run "echo on all; test bytecode.tst" to recreate.
          }

        m_tw->set_echo_file_pos (line + 1);
      }
bail_echo:

    // TODO: Check all trees one time and cache the result somewhere?
    //       Until another bp is set? Debugging will be quite slow
    //       with one check for each op-code.

    if (m_tw->debug_mode ())
      {
        auto it = unwind_data->m_ip_to_tree.find (tmp_ip);

        if (it == unwind_data->m_ip_to_tree.end ())
          goto debug_check_end;

        bool is_ret = *ip == static_cast<unsigned char> (INSTR::RET) || *ip == static_cast<unsigned char> (INSTR::RET_ANON);

        m_sp = sp;
        m_bsp = bsp;
        m_rsp = rsp;
        m_code = code;
        m_data = data;
        m_name_data = name_data;
        m_ip = tmp_ip;
        m_unwind_data = unwind_data;
        m_tw->set_active_bytecode_ip (tmp_ip);

        tree *t = it->second;

        // do_breakpoint will check if there is a breakpoint attached
        // to the relevant code and escape to the debugger repl
        // if neccessary.
        if (t)
          {
            try
              {
                m_tw->do_breakpoint (t->is_active_breakpoint (*m_tw), is_ret);
              }
            CATCH_INTERRUPT_EXCEPTION
            CATCH_INDEX_EXCEPTION
            CATCH_EXECUTION_EXCEPTION
            CATCH_BAD_ALLOC
            CATCH_EXIT_EXCEPTION
            catch (const quit_debug_exception &qde)
              {
                (*sp++).i = qde.all ();
                (*sp++).i = static_cast<int>(error_type::DEBUG_QUIT);
                goto unwind;
              }
          }
      }
  }
  debug_check_end:
  {
    if (OCTAVE_UNLIKELY (m_profiler_enabled))
      {
        auto p = m_vm_profiler;

        if (p)
          {
            std::string fn_name = data[2].string_value (); // profiler_name () querried at compile time
            vm_profiler::vm_profiler_fn_stats &stat = m_vm_profiler->m_map_fn_stats[fn_name];

            // If someone enabled profiling in the debugger we need to wait until
            // the debug_check: block is ran next time.
            if (stat.m_v_t.size())
              {
                int tmp_ip = ip - code;
                if (onebyte_op)
                  tmp_ip--;
                stat.m_v_ip.back () = tmp_ip; // Sets a new 'currently running ip'
                stat.m_v_t.back () = vm_profiler::unow (); // Sets a new timestamp for the current ip
              }
          }
      }
  }
  if (onebyte_op)
    {
      int opcode = ip[-1];
      arg0 = ip[0];
      ip++;
      goto *instr [opcode];
    }
  else
    {
      int opcode = ip[0];
      arg0 = ip[1];
      ip += 2;
      goto *instr [opcode];
    }
}

debug: // TODO: Remove
  {
    if (m_tw->debug_mode ())
      {
        m_ip = ip - code;
        m_sp = sp;
        m_tw->set_active_bytecode_ip (ip - code);

        try
          {
            m_tw->enter_debugger ();
          }
        CATCH_INTERRUPT_EXCEPTION
        CATCH_INDEX_EXCEPTION
        CATCH_EXECUTION_EXCEPTION
        CATCH_BAD_ALLOC
        CATCH_EXIT_EXCEPTION
      }
  }
  DISPATCH ();

  wide:
  {
    int opcode = arg0; // The opcode to execute next is in arg0, i.e. ip[-1]
    // The next opcode needs its arg0, which is a unsigned short instead of the usual byte
    // that DISPATCH() writes to arg0.
    arg0 = USHORT_FROM_UCHAR_PTR (ip);
    ip += 2; // Forward ip so it points to after the widened argument
    goto *instr [opcode];
  }

  ext_nargout:
  {
    // This opcode replaces the first opcode argument of the next opcode, with the
    // current function's nargout.
    //
    // Anonymous functions need to have a dynamic "expression nargout" on the
    // root expression since the "expression nargout" is decided by the caller.
    // E.g. '[a b] = anon ()' yields 2 for the root expression in 'anon'.
    //
    // In a ordinary function "expression nargout" is decided by the source row.

    int opcode = arg0; // The opcode to execute next is in arg0, i.e. ip[-1]
    // The next opcode needs its arg0, which is supposed to be a nargout value
    arg0 = bsp[0].i; // %nargout is stored in the first slot in the stack frame
    ip++; // Forward ip so it points to after the nargout argument in the next opcode
    goto *instr [opcode]; // Execute the next opcode
  }

  dup_move:
  {
    // Copy the top of the stack and move it n positions down the stack.
    int n = arg0;

    octave_value ov = sp[-1].ov;
    sp[-1 - n].ov = ov;
  }
  DISPATCH ();

  enter_script_frame:
  {
    auto fp = m_tw->get_current_stack_frame ();
    fp->vm_enter_script ();
  }
  DISPATCH_1BYTEOP ();
  exit_script_frame:
  {
    auto fp = m_tw->get_current_stack_frame ();
    fp->vm_exit_script ();
  }
  DISPATCH_1BYTEOP ();

  enter_nested_frame:
  {
    auto fp = m_tw->get_current_stack_frame ();
    fp->vm_enter_nested ();
  }
  DISPATCH_1BYTEOP ();

  install_function:
  {
    int slot = arg0;
    int fn_cst_idx = POP_CODE_INT ();

    std::string fn_name = name_data [slot];

    octave_value fn = data[fn_cst_idx];

    symbol_table& symtab = m_tw->get_interpreter ().get_symbol_table ();
    symtab.install_cmdline_function (fn_name, fn);

    // Make sure that any variable with the same name as the new
    // function is cleared.
    octave_value &ov = bsp[slot].ov;

    if (ov.is_ref ())
      ov.ref_rep ()->set_value (octave_value {});
    else
      ov = octave_value {};
  }
  DISPATCH ();

  mul_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_mul, mul_cst_dbl, MUL_CST_DBL);
  DISPATCH ();
  mul_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_mul, mul_cst, MUL_CST, m_scalar_typeid);
  DISPATCH ();
  add_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_add, add_cst_dbl, ADD_CST_DBL);
  DISPATCH ();
  add_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_add, add_cst, ADD_CST, m_scalar_typeid);
  DISPATCH ();
  div_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_div, div_cst_dbl, DIV_CST_DBL);
  DISPATCH ();
  div_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_div, div_cst, DIV_CST, m_scalar_typeid);
  DISPATCH ();
  sub_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_sub, sub_cst_dbl, SUB_CST_DBL);
  DISPATCH ();
  sub_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_sub, sub_cst, SUB_CST, m_scalar_typeid);
  DISPATCH ();
  le_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_lt, le_cst_dbl, LE_CST_DBL);
  DISPATCH ();
  le_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_le, le_cst, LE_CST, m_scalar_typeid);
  DISPATCH ();
  le_eq_cst:
  MAKE_BINOP_CST_SELFMODIFYING (binary_op::op_le, le_eq_cst_dbl, LE_EQ_CST_DBL);
  DISPATCH ();
  le_eq_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_le_eq, le_eq_cst, LE_EQ_CST, m_scalar_typeid);
  DISPATCH ();
  gr_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_gr, gr_cst, GR_CST, m_scalar_typeid)
  DISPATCH ();
  gr_cst:
  MAKE_BINOP_CST_SELFMODIFYING(binary_op::op_gt, gr_cst_dbl, GR_CST_DBL)
  DISPATCH();
  gr_eq_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_gr_eq, gr_eq_cst, GR_EQ_CST, m_scalar_typeid)
  DISPATCH();
  gr_eq_cst:
  MAKE_BINOP_CST_SELFMODIFYING(binary_op::op_ge, gr_eq_cst_dbl, GR_EQ_CST_DBL)
  DISPATCH();
  eq_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED(m_fn_dbl_eq, eq_cst, EQ_CST, m_scalar_typeid)
  DISPATCH();
  eq_cst:
  MAKE_BINOP_CST_SELFMODIFYING(binary_op::op_eq, eq_cst_dbl, EQ_CST_DBL)
  DISPATCH();
  neq_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED(m_fn_dbl_neq, neq_cst, NEQ_CST, m_scalar_typeid)
  DISPATCH();
  neq_cst:
  MAKE_BINOP_CST_SELFMODIFYING(binary_op::op_ne, neq_cst_dbl, NEQ_CST_DBL)
  DISPATCH();
  pow_cst_dbl:
  MAKE_BINOP_CST_SPECIALIZED (m_fn_dbl_pow, pow_cst, POW_CST, m_scalar_typeid)
  DISPATCH();
  pow_cst:
  MAKE_BINOP_CST_SELFMODIFYING(binary_op::op_pow, pow_cst_dbl, POW_CST_DBL)
  DISPATCH();

  index_struct_subcall:
  {
    // The INDEX_STRUCT_SUBCALL opcode is a chain of INDEX_STRUCT_SUBCALL opcodes,
    // that are always proceded by INDEX_STRUCT_CALL.
    //
    // INDEX_STRUCT_SUBCALL executes a subexpression of a chain of subsrefs.
    //
    // The opcode is more or less a "stackification" of tree_index_expression::evaluate_n ().

    int nargout = arg0;
    // TODO: Kludge alert. Mirror the behaviour in ov_classdef::subsref
    // where under certain conditions a magic number nargout of -1 is
    // expected to  maybe return a cs-list. "-1" in this context
    // does not have the same meaning as in the VM, where it means
    // a varargout with only one return symbol 'varargout'.
    int subsref_nargout = nargout;
    if (nargout == 255)
      {
        nargout = 1;
        subsref_nargout = -1;
      }
    int i = *ip++;
    int n = *ip++;
    int n_args_on_stack = *ip++;
    char type = *ip++;

    // The object to index is before the args on the stack
    octave_value &ov = (sp[-1 - n_args_on_stack]).ov;

    bool ov_is_vm_chainargs_wrapper = ov.is_vm_chainargs_wrapper ();
    bool need_stepwise_subsref = ov_need_stepwise_subsrefs(ov);

    try
      {
        if (!ov_is_vm_chainargs_wrapper && need_stepwise_subsref)
          {
            switch (ov.vm_dispatch_call ())
              {
                case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
                  {
                    // The last iteration the caller wants as many returned on the stack
                    // as it wants the callee to produce.
                    // In any other iteration the caller wants one value returned, but still
                    // wants the callee to produce nargout number of return values.
                    int caller_nvalback;
                    if (i + 1 != n)
                      caller_nvalback = 1;
                    else
                      caller_nvalback = nargout;
                    
                    (*sp++).i = n_args_on_stack;
                    (*sp++).i = nargout;
                    (*sp++).i = caller_nvalback;
                    (*sp++).i = 0;
                    goto make_nested_handle_call;
                  }
                case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
                  {
                    (*sp++).pee = new execution_exception {"error", "", "invalid undefined value in chained index expression"}; // TODO: Uninformative?
                    (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
                    goto unwind;
                  }
                case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
                  {
                    octave_value_list ovl;
                    // The operands are on the top of the stack
                    POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                    CHECK_PANIC (! ov.is_function () || ov.is_classdef_meta ()); // TODO: Remove

                    octave_value_list retval;
                    try
                      {
                        m_tw->set_active_bytecode_ip (ip - code);
                        retval = ov.simple_subsref (type, ovl, subsref_nargout);
                      }
                    CATCH_INTERRUPT_EXCEPTION
                    catch (index_exception& ie)
                      {
                        // Fetch the name of the left most object being indexed from the arg name entries.
                        int ip_offset = ip - code;
                        arg_name_entry entry = get_argname_entry (ip_offset, unwind_data);

                        if (entry.m_obj_name != "")
                          {
                            // Only set the name if the object is defined (i.e. not a function call to e.g. 'zero')
                            // to match the error messages in tree_evaluator and pass index.tst
                            if (m_tw->get_current_stack_frame ()->varval (entry.m_obj_name).is_defined ())
                              ie.set_var (entry.m_obj_name);
                          }

                        (*sp++).pee = ie.dup ();
                        (*sp++).i = static_cast<int> (error_type::INDEX_ERROR);
                        goto unwind;
                      }
                    CATCH_EXECUTION_EXCEPTION
                    CATCH_BAD_ALLOC
                    CATCH_EXIT_EXCEPTION

                    ov = octave_value ();
                    STACK_DESTROY (1); // Destroy the object being indexed

                    ovl.clear (); // Destroy the args

                    bool is_last_iteration = i + 1 == n;

                    if (is_last_iteration)
                      {
                        // TODO: Kludge for e.g. "m = containsers.Map;" which returns a function.
                        //       Should preferably be done by .subsref?
                        //       If only classdefs does this, this is not really needed here.
                        octave_value val = (retval.length () ? retval(0) : octave_value ());
                        if (val.is_function ())
                          {
                            octave_function *fcn = val.function_value (true);

                            if (fcn)
                              {
                                retval = fcn->call (*m_tw, nargout, {});
                              }
                          }
                      }

                    // Push one value if this iteration of INDEX_STRUCT_SUBCALL
                    // is not the last one.
                    if (!is_last_iteration)
                      nargout = 1;
                    EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
                  }
                break;

                case octave_base_value::vm_call_dispatch_type::OCT_CALL:
                case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
                case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
                  {
                    CHECK_PANIC (ov.has_function_cache ()); // TODO :Remove

                    octave_function *fcn;
                    try
                      {
                        stack_element *first_arg = &sp[-n_args_on_stack];
                        stack_element *end_arg = &sp[0];
                        fcn = ov.get_cached_fcn (first_arg, end_arg);
                      }
                    CATCH_EXECUTION_EXCEPTION // parse errors might throw in classdefs

                    if (! fcn)
                      {
                        (*sp++).pee = new execution_exception {"error", "", "invalid return value in chained index expression"}; // TODO: Uninformative?
                        (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
                        goto unwind;
                      }
                    else if (fcn->is_compiled ())
                      {
                        octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);

                        // The last iteration the caller wants as many returned on the stack
                        // as it wants the callee to produce.
                        // In any other iteration the caller wants one value returned, but still
                        // wants the callee to produce nargout number of return values.
                        int caller_nvalback;
                        if (i + 1 != n)
                          caller_nvalback = 1;
                        else
                          caller_nvalback = nargout;

                        MAKE_BYTECODE_CALL

                        // Now dispatch to first instruction in the
                        // called function
                      }
                    else
                      {
                        try
                          {
                            octave_value_list ovl;
                            // The operands are on the top of the stack
                            POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

                            m_tw->set_active_bytecode_ip (ip - code);
                            octave_value_list ret = fcn->call (*m_tw, nargout, ovl);

                            STACK_DESTROY (1); // Destroy the object being indexed

                            bool is_last_iteration = i + 1 == n;

                            if (is_last_iteration)
                              {
                                // TODO: Kludge for e.g. "m = containsers.Map;" which returns a function.
                                //       Should preferably be done by .subsref?
                                //       If only classdefs does this, this is not really needed here.
                                octave_value val = (ret.length () ? ret(0) : octave_value ());
                                if (val.is_function ())
                                  {
                                    octave_function *fcn_final = val.function_value (true);

                                    if (fcn_final)
                                      {
                                        ret = fcn_final->call (*m_tw, nargout, {});
                                      }
                                  }
                              }

                            // Push one value if this iteration of INDEX_STRUCT_SUBCALL
                            // is not the last one.
                            if (!is_last_iteration)
                              nargout = 1;
                            EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                          }
                        CATCH_INTERRUPT_EXCEPTION
                        CATCH_INDEX_EXCEPTION
                        CATCH_EXECUTION_EXCEPTION
                        CATCH_BAD_ALLOC
                        CATCH_EXIT_EXCEPTION
                      }
                  }
                break;
              }

          }
        else
          {
            if (!ov_is_vm_chainargs_wrapper)
              {
                // Replace the object being indexed on the stack with a wrapper object
                // with the object being indexed in it.
                ov = octave_value {new octave_vm_chainargs_wrapper {ov}};
              }

            octave_vm_chainargs_wrapper &ovb_chainargs = REP (octave_vm_chainargs_wrapper, ov);

            { // Limit ovl to this scope to not polute the whole C++ scope with an invalid ovl after the move
              octave_value_list ovl;
              // The operands are on the top of the stack
              POP_STACK_RANGE_TO_OVL (ovl, sp - n_args_on_stack, sp);

              ovb_chainargs.append_args (std::move (ovl));
              ovb_chainargs.append_type (type);
            }

            // In the last INDEX_STRUCT_SUBCALL the args have been collected and it is
            // time to make the call.
            if (i + 1 == n)
              {
                std::list<octave_value_list> idxs = ovb_chainargs.steal_idxs ();
                std::string types = ovb_chainargs.steal_types ();

                // Replace the wrapper object with the object being indexed.
                // Since ov and ovb_chainargs are the same octave value
                // there has to be a intermediate copy to keep the refcounts
                // correct.
                {
                  octave_value tmp = ovb_chainargs.steal_obj_to_call ();
                  ov = tmp;
                  // Note: 'ovb_chainargs' is invalid from now on
                }

                switch (ov.vm_dispatch_call ())
                  {
                    case octave_base_value::vm_call_dispatch_type::OCT_NESTED_HANDLE:
                    case octave_base_value::vm_call_dispatch_type::OCT_FN_LOOKUP:
                      PANIC ("Invalid dispatch");
                      break;
                    case octave_base_value::vm_call_dispatch_type::OCT_SUBSREF:
                      {
                        CHECK_PANIC (! ov.is_function () || ov.is_classdef_meta ()); // TODO: Remove

                        octave_value_list retval;
                        try
                          {
                            m_tw->set_active_bytecode_ip (ip - code);
                            retval = ov.subsref (types, idxs, subsref_nargout);
                            idxs.clear ();
                          }
                        CATCH_INTERRUPT_EXCEPTION
                        catch (index_exception& ie)
                          {
                            // Fetch the name of the left most object being indexed from the arg name entries.
                            int ip_offset = ip - code - 1; // Minus one since ip points at next opcode now
                            arg_name_entry entry = get_argname_entry (ip_offset, unwind_data);

                            if (entry.m_obj_name != "")
                              {
                                // Only set the name if the object is defined (i.e. not a function call to e.g. 'zero')
                                // to match the error messages in tree_evaluator and pass index.tst
                                if (m_tw->get_current_stack_frame ()->varval (entry.m_obj_name).is_defined ())
                                  ie.set_var (entry.m_obj_name);
                              }

                            (*sp++).pee = ie.dup ();
                            (*sp++).i = static_cast<int> (error_type::INDEX_ERROR);
                            goto unwind;
                          }
                        CATCH_EXECUTION_EXCEPTION
                        CATCH_BAD_ALLOC
                        CATCH_EXIT_EXCEPTION

                        // TODO: Kludge for e.g. "m = containsers.Map;" which returns a function.
                        //       Should preferably be done by subsref()/call()?
                        octave_value val = (retval.length () ? retval(0) : octave_value ());
                        if (val.is_function ())
                          {
                            octave_function *fcn_final = val.function_value (true);

                            if (fcn_final)
                              {
                                retval = fcn_final->call (*m_tw, nargout, {});
                              }
                          }

                        STACK_DESTROY (1); // Destroy the object being indexed
                        EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (retval, nargout);
                      }
                    break;

                    case octave_base_value::vm_call_dispatch_type::OCT_CALL:
                    case octave_base_value::vm_call_dispatch_type::OCT_HANDLE:
                    case octave_base_value::vm_call_dispatch_type::OCT_OBJECT:
                      {
                        CHECK_PANIC (ov.is_function ()); // TODO :Remove

                        octave_function *fcn = ov.function_value ();

                        // tree_evaluator silently ignored nullptr here
                        if (! fcn)
                          {
                            (*sp++).pee = new execution_exception {"error", "", "invalid return value in chained index expression"}; // TODO: Uninformative?
                            (*sp++).i = static_cast<int> (error_type::EXECUTION_EXC);
                            goto unwind;
                          }
#if 0 // TODO: Support for bytecode calls to ctors
                    else if (fcn->is_compiled ())
                      {
                        octave_user_code *usr_fcn = static_cast<octave_user_code *> (fcn);

                        // Alot of code in this define
                        int nargout = 1;
                        MAKE_BYTECODE_CALL

                        // Now dispatch to first instruction in the
                        // called function
                      }
#endif
                        else
                          {
                            try
                              {
                                if (idxs.size () != 1)
                                  error ("unexpected extra index at end of expression");
                                if (type != '(')
                                  error ("invalid index type '%c' for function call",
                                    type);

                                octave_value_list final_args = idxs.front ();

                                m_tw->set_active_bytecode_ip (ip - code);
                                octave_value_list ret = fcn->call (*m_tw, nargout, final_args);

                                // TODO: Kludge for e.g. "m = containsers.Map;" which returns a function.
                                //       Should preferably be done by subsref()/call()?
                                //       Is this actually executed? See pt-idx.cc
                                octave_value val = (ret.length () ? ret(0) : octave_value ());
                                if (val.is_function ())
                                  {
                                    octave_function *fcn_final = val.function_value (true);

                                    if (fcn_final)
                                      {
                                        ret = fcn_final->call (*m_tw, nargout, final_args); // Called with same args as above ...
                                      }
                                  }

                                STACK_DESTROY (1); // Destroy the object being indexed
                                EXPAND_CSLIST_PUSH_N_OVL_ELEMENTS_TO_STACK (ret, nargout);
                              }
                            CATCH_INTERRUPT_EXCEPTION
                            CATCH_INDEX_EXCEPTION
                            CATCH_EXECUTION_EXCEPTION
                            CATCH_BAD_ALLOC
                            CATCH_EXIT_EXCEPTION
                          }
                      }
                    break;
                  }
              }
          }
      }
    CATCH_INTERRUPT_EXCEPTION
    CATCH_INDEX_EXCEPTION
    CATCH_EXECUTION_EXCEPTION
    CATCH_BAD_ALLOC
    CATCH_EXIT_EXCEPTION
  }
  DISPATCH ();
}

octave_value
vm::handle_object_end (octave_value ov, int idx, int nargs)
{
  // See tree_evaluator::evaluate_end_expression()
  octave_value ans;

  auto &interpreter = m_tw->get_interpreter ();
  std::string dispatch_class = ov.class_name ();
  symbol_table& symtab = interpreter.get_symbol_table ();

  octave_value meth = symtab.find_method ("end", dispatch_class);

  if (meth.is_defined ())
    ans = interpreter.feval (meth, ovl (ov, idx+1, nargs), 1).first_or_nil_ov ();
  else
    ans = octave_value (ov.end_index (idx, nargs));

  return ans;
}

octave_value
vm::find_fcn_for_cmd_call (std::string *name)
{
  interpreter& interp = __get_interpreter__();

  symbol_table& symtab = interp.get_symbol_table ();

  return symtab.find_function (*name);
}

vm::error_data
vm::handle_error (error_type error_type)
{
  error_data ret;

  error_system& es = m_tw->get_interpreter().get_error_system ();

  std::stringstream ss;
  // ip points to the "next" instruction, so search for the
  // code location for ip - 1
  loc_entry loc = find_loc (m_ip - 1, m_unwind_data->m_loc_entry);

  switch (error_type)
    {
    case error_type::BAD_ALLOC:
      {
        execution_exception e {"error", "Octave:bad-alloc", "out of memory or dimension too large for Octave's index type"};
        es.save_exception (e);

        break;
      }
    case error_type::ID_UNDEFINED:
      {
        std::string *sp = m_sp [-1].ps;
        m_sp--;
        std::string id_name = *sp;
        delete sp;

        ss << "'" << id_name << "'" <<
          " undefined near line " << loc.m_line <<
          ", column " << loc.m_col;

        execution_exception e { "error",
          "Octave:undefined-function",
          ss.str ()};

        // Since the exception was made in the VM it has not been saved yet
        es.save_exception (e);

        break;
      }
    case error_type::IF_UNDEFINED:
      {
        // error ("%s: undefined value used in conditional expression", warn_for);
        ss << "if's condition undefined near line " <<
          loc.m_line << ", column " << loc.m_col;

        execution_exception e {"error", "", ss.str ()};

        es.save_exception (e);

        break;
      }
    case error_type::INDEX_ERROR:
      {
        execution_exception *e = m_sp [-1].pee;

        CHECK (e);
        es.save_exception (*e);

        delete e;

        m_sp--;

        break;
      }
    case error_type::EXECUTION_EXC:
      {
        execution_exception *e = m_sp [-1].pee;

        CHECK (e);
        es.save_exception (*e);

        delete e;

        m_sp--;

        break;
      }
    case error_type::INTERRUPT_EXC:
      break; // Do nothing
    case error_type::EXIT_EXCEPTION:
      ret.m_safe_to_return = (--m_sp)->i;
      ret.m_exit_status = (--m_sp)->i;
      break;
    case error_type::INVALID_N_EL_RHS_IN_ASSIGNMENT:
    {
      execution_exception e {"error", "", "invalid number of elements on RHS of assignment"};

      es.save_exception (e);

      break;
    }
    case error_type::RHS_UNDEF_IN_ASSIGNMENT:
    {
      execution_exception e {"error", "", "value on right hand side of assignment is undefined"};

      es.save_exception (e);

      break;
    }
    case error_type::DEBUG_QUIT:
    {
      ret.m_debug_quit_all = m_sp[-1].i;
      m_sp--;

      break;
    }
    default:
      TODO ("Unhandeled error type");
    }

  return ret;
}

vm::~vm ()
{
  delete [] m_stack0;

  CHECK (m_output_ignore_data == nullptr);
}

vm::vm (tree_evaluator *tw, bytecode &initial_bytecode)
{
  m_ti = &__get_type_info__();
  m_stack0 = new stack_element[stack_size + stack_pad * 2];

  for (unsigned i = 0; i < stack_pad; i++)
    {
      m_stack0[i].u = stack_magic_int;
      m_stack0[i + stack_size].u = stack_magic_int;
    }

  m_sp = m_stack = m_stack0 + stack_pad;
  m_tw = tw;
  m_symtab = &__get_symbol_table__();

  m_data = initial_bytecode.m_data.data ();
  m_code = initial_bytecode.m_code.data ();
  m_name_data = initial_bytecode.m_ids.data ();
  m_unwind_data = &initial_bytecode.m_unwind_data;

  // Check that the typeids are what the VM anticipates. If the id change, just change
  // the constexpr.
  CHECK (octave_scalar::static_type_id () == m_scalar_typeid);
  CHECK (octave_bool::static_type_id () == m_bool_typeid);
  CHECK (octave_matrix::static_type_id () == m_matrix_typeid);
  CHECK (octave_cs_list::static_type_id () == m_cslist_typeid);

  // Function pointer used for specialized op-codes
  m_fn_dbl_mul = m_ti->lookup_binary_op (octave_value::binary_op::op_mul, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_div = m_ti->lookup_binary_op (octave_value::binary_op::op_div, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_add = m_ti->lookup_binary_op (octave_value::binary_op::op_add, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_sub = m_ti->lookup_binary_op (octave_value::binary_op::op_sub, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_pow = m_ti->lookup_binary_op (octave_value::binary_op::op_pow, m_scalar_typeid, m_scalar_typeid);

  m_fn_dbl_le = m_ti->lookup_binary_op (octave_value::binary_op::op_lt, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_le_eq = m_ti->lookup_binary_op (octave_value::binary_op::op_le, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_gr = m_ti->lookup_binary_op (octave_value::binary_op::op_gt, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_gr_eq = m_ti->lookup_binary_op (octave_value::binary_op::op_ge, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_eq = m_ti->lookup_binary_op (octave_value::binary_op::op_eq, m_scalar_typeid, m_scalar_typeid);
  m_fn_dbl_neq = m_ti->lookup_binary_op (octave_value::binary_op::op_ne, m_scalar_typeid, m_scalar_typeid);

  m_fn_dbl_usub = m_ti->lookup_unary_op (octave_value::unary_op::op_uminus, m_scalar_typeid);
  m_fn_dbl_not = m_ti->lookup_unary_op (octave_value::unary_op::op_not, m_scalar_typeid);
  m_fn_bool_not = m_ti->lookup_unary_op (octave_value::unary_op::op_not, m_bool_typeid);

  m_pi_builtin_fn = m_symtab->find_built_in_function ("pi").function_value ();
  m_i_builtin_fn = m_symtab->find_built_in_function ("i").function_value ();
  m_e_builtin_fn = m_symtab->find_built_in_function ("e").function_value ();
  // If the platform has no M_PI, M_E we need to initialize ov_pi and ov_e
#if !defined (M_PI)
  ov_pi = 4.0 * atan (1.0);
#endif
#if !defined (M_E)
  ov_e = exp (1.0);
#endif

}

// If there are too many return values we can't just move them since the stacks will overlap so we
// need to copy the args first with this proc
static void copy_many_args_to_caller (octave::stack_element *sp,
                                      octave::stack_element *caller_stack_end,
                                      int n_args_to_move, int n_args_caller_expects)
{
  // Move args to an ovl
  octave_value_list ovl;
  for (int i = 0; i < n_args_to_move; i++)
    {
      octave_value &arg = caller_stack_end[i].ov;
      ovl.append (std::move (arg));
    }

  for (int i = 0; i < n_args_to_move; i++)
    {
      PUSH_OV (ovl(n_args_to_move - 1 - i)); // backwards
    }

  // Push missing args
  for (int i = n_args_to_move; i < n_args_caller_expects; i++)
    PUSH_OV ();
}

static octave_value xeval_for_numel (octave_value &ov, const std::string& type, const std::list<octave_value_list>& idx);

// This function reimplements octave_lvalue::numel()
// TODO: octave_lvalue::numel() could be broken out or made static and used instead. But don't mess with that code
//       to keep the VM somewhat independent of other code.
static int lhs_assign_numel (octave_value &ov, const std::string& type, const std::list<octave_value_list>& idx)
{
  // Return 1 if there is no index because without an index there
  // should be no way to have a cs-list here.  Cs-lists may be passed
  // around internally but they are not supposed to be stored as
  // single symbols in a stack frame.

  std::size_t num_indices = idx.size ();

  if (num_indices == 0)
    return 1;

  switch (type[num_indices-1])
    {
    case '(':
      return 1;

    case '{':
      {
        // FIXME: Duplicate code in '.' case below...

        // Evaluate, skipping the last index.

        std::string tmp_type = type;
        std::list<octave_value_list> tmp_idx = idx;

        tmp_type.pop_back ();
        tmp_idx.pop_back ();

        octave_value tmp = xeval_for_numel (ov, tmp_type, tmp_idx);

        octave_value_list tidx = idx.back ();

        if (tmp.is_undefined ())
          {
            if (tidx.has_magic_colon ())
              err_invalid_inquiry_subscript ();

            tmp = Cell ();
          }
        else if (tmp.is_zero_by_zero ()
                 && (tmp.is_matrix_type () || tmp.is_string ()))
          {
            tmp = Cell ();
          }

        return tmp.xnumel (tidx);
      }
      break;

    case '.':
      {
        // Evaluate, skipping either the last index or the last two
        // indices if we are looking at "(idx).field".

        std::string tmp_type = type;
        std::list<octave_value_list> tmp_idx = idx;

        tmp_type.pop_back ();
        tmp_idx.pop_back ();

        bool paren_dot = num_indices > 1 && type[num_indices-2] == '(';

        // Index for paren operator, if any.
        octave_value_list pidx;

        if (paren_dot)
          {
            pidx = tmp_idx.back ();

            tmp_type.pop_back ();
            tmp_idx.pop_back ();
          }

        octave_value tmp = xeval_for_numel (ov, tmp_type, tmp_idx);

        bool autoconv = (tmp.is_zero_by_zero ()
                         && (tmp.is_matrix_type () || tmp.is_string ()
                             || tmp.iscell ()));

        if (paren_dot)
          {
            // Use octave_map, not octave_scalar_map so that the
            // dimensions are 0x0, not 1x1.

            if (tmp.is_undefined ())
              {
                if (pidx.has_magic_colon ())
                  err_invalid_inquiry_subscript ();

                tmp = octave_map ();
              }
            else if (autoconv)
              tmp = octave_map ();

            return tmp.xnumel (pidx);
          }
        else if (tmp.is_undefined () || autoconv)
          return 1;
        else
          return tmp.xnumel (octave_value_list ());
      }
      break;

    default:
      panic_impossible ();
    }
}

static octave_value xeval_for_numel (octave_value &ov, const std::string& type, const std::list<octave_value_list>& idx)
{
  octave_value retval;

  try
    {
      retval = ov;

      if (retval.is_constant () && ! idx.empty ())
        retval = retval.subsref (type, idx);
    }
  catch (const execution_exception&)
    {
      // Ignore an error and treat it as undefined.  The error
      // could happen because there is an index is out of range
      // and we will be resizing a cell array.

      interpreter& interp = __get_interpreter__ ();

      interp.recover_from_exception ();

      retval = octave_value ();
    }

  return retval;
}


loc_entry vm::find_loc (int ip, std::vector<octave::loc_entry> &loc_entries)
{
  int best = -1;

  int n = loc_entries.size ();

  // TODO: Should maybe be some binary search, but only called in
  //       exceptions so who cares?
  for (int i = 0; i < n; i++)
    {
      loc_entry &e = loc_entries[i];

      if (ip >= e.m_ip_start && ip < e.m_ip_end)
        best = i;
    }

  if (best == -1)
    return {};

  return loc_entries[best];
}

void vm::set_nargin (int nargin)
{
  m_tw->set_nargin (nargin);
}

void vm::caller_ignores_output ()
{
  m_output_ignore_data = new output_ignore_data;
  m_output_ignore_data->m_v_lvalue_list.back () = m_tw->lvalue_list ();
  m_output_ignore_data->m_v_owns_lvalue_list.back () = false;
  m_output_ignore_data->m_external_root_ignorer = true;
}

void vm::set_nargout (int nargout)
{
  m_tw->set_nargout (nargout);
}

int
vm::find_unwind_entry_for_forloop (int current_stack_depth)
{
  int best_match = -1;

  // Find a for loop entry that matches the current instruction pointer
  // range and also got an anticipated stack depth less than current stack
  // depth.
  //
  // I.e. if the ip is in a for loop, we want to unwind down the stack
  // untill we reach the stack depth of the for loop to be able to remove
  // its native int:s properly.
  //
  // To be able to unwind nested for loops we look for smaller and
  // smaller stack depths given by current_stack_depth parameter.

  for (unsigned i = 0; i < m_unwind_data->m_unwind_entries.size(); i++)
    {
      unwind_entry& entry = m_unwind_data->m_unwind_entries[i];
      int start = entry.m_ip_start;
      int end = entry.m_ip_end;
      int stack_depth = entry.m_stack_depth;

      // Skip not for loop entries
      if (entry.m_unwind_entry_type != unwind_entry_type::FOR_LOOP)
        continue;
      // Are ip the range?
      if (start > m_ip || end <= m_ip)
        continue;
      // Is the stack depth ok?
      if (stack_depth >= current_stack_depth)
        continue;

      // Is it better than prior match?
      if (best_match != -1)
        {
          if (best_match > stack_depth)
            continue;
        }

      best_match = stack_depth;
    }

  return best_match;
}

unwind_entry*
vm::find_unwind_entry_for_current_state (bool only_find_unwind_protect)
{
  int best_match = -1;

  // Find the entry with the highest start instruction offset
  for (unsigned i = 0; i < m_unwind_data->m_unwind_entries.size(); i++)
    {
      unwind_entry& entry = m_unwind_data->m_unwind_entries[i];
      int start = entry.m_ip_start;
      int end = entry.m_ip_end;

      // When unwinding for e.g. interrupt exceptions we are only looking for UNWIND_PROTECT
      if (only_find_unwind_protect && (entry.m_unwind_entry_type != unwind_entry_type::UNWIND_PROTECT))
        continue;

      // Skip for loop entries
      if (entry.m_unwind_entry_type == unwind_entry_type::FOR_LOOP)
        continue;

      // Are ip the range?
      if (start > m_ip || end <= m_ip) // TODO: end < m_ip ???
        continue;

      // Is it better than prior match?
      if (best_match != -1)
        {
          int best_start =
            m_unwind_data->m_unwind_entries[best_match].m_ip_start;
          if (best_start > start)
            continue;
        }

      best_match = i;
    }

  if (best_match == -1)
    return nullptr;

  return &m_unwind_data->m_unwind_entries[best_match];
}

static bool ov_need_stepwise_subsrefs (octave_value &ov)
{
  return !ov.isobject () && !ov.isjava () && !(ov.is_classdef_meta () && ! ov.is_package ());
}

int64_t
vm_profiler::unow ()
{
  return octave_gettime_ns_wrapper ();
}

void
vm_profiler::vm_profiler_fn_stats::add_t (int64_t dt)
{
  int ip = m_v_ip.back ();
  maybe_resize (ip);

  m_v_cum_t[ip] += dt;
  ++m_v_n_cum[ip];
}

void
vm_profiler::add_t (int64_t dt)
{
  if (!m_shadow_call_stack.size ())
    return;

  m_shadow_call_stack.back ().m_t_self_cum += dt;
}

// There is no std::format since we use C++ 11 so lets make our own.
// The 'format' attribute gives nice compiler warnings on missuse.
static
std::string
x_snprintf (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

static
std::string
x_snprintf (const char *fmt, ...)
{
    int n = 32;
    do {
      char *buff = new char[n];

      va_list va;
      va_start (va, fmt);
      int n_needed = vsnprintf (buff, n, fmt, va);
      va_end (va);

      std::string ret;

      try
        {
          std::string tmp {buff};
          ret = tmp;
        }
      catch (...) // Maybe bad_alloc could be thrown
        {
          delete [] buff;
          throw;
        }

      delete [] buff;

      if (n_needed < 0)
        error ("profiler internal error: Invalid call to x_snprintf()");
      if (n_needed < n)
        return ret;

      n = n_needed + 1;
    } while (1);
}

void
vm_profiler::print_to_stdout ()
{
  using std::string;
  using std::vector;
  using std::map;
  using std::pair;

  using std::cout;
  using std::setw;

  // These could probably be vectors, but we'll do with maps to keep the
  // code easier to follow.
  map<string, int64_t> map_fn_to_cum_t;
  map<string, int64_t> map_fn_to_self_cum_t;
  map<string, vector<string>> map_fn_to_sourcerows;
  map<string, vector<pair<int, string>>> map_fn_to_opcodes_stringrows;
  map<string, string> map_fn_to_annotated_source;
  map<string, string> map_fn_to_annotated_bytecode;

  // Calculate cumulative function time
  for (auto kv : m_map_fn_stats)
    {
      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;

      int64_t t_fn_cum = 0;
      int64_t t_fn_self_cum = 0;
      unsigned n = stats.m_v_cum_t.size ();

      for (unsigned ip = 0; ip < n; ip++)
        {
          t_fn_cum += stats.m_v_cum_t[ip];
          t_fn_self_cum += stats.m_v_cum_t[ip];
        }
      for (unsigned ip = 0; ip < stats.m_v_cum_call_t.size (); ip++)
        t_fn_cum += stats.m_v_cum_call_t[ip];

      map_fn_to_cum_t[fn_name] = t_fn_cum;
      map_fn_to_self_cum_t[fn_name] = t_fn_self_cum;
    }

  // Try to get the source code
  for (auto kv : m_map_fn_stats)
    {
      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;
      string file = stats.m_fn_file;

      auto &interp = __get_interpreter__ ();

      // Call type with the quiet flag to get the source
      // Also works for functions without source code in files.
      octave_value_list ans;
      string source_text;

      bool got_source_text = false;

      if (!got_source_text)
        {
          octave_value_list args;
          args.append ("-q");
          args.append (file);
          try
            {
              if (file.size ())
                ans = interp.feval ("type", args, 1);
            }
          catch (execution_exception &)
            {
              // Didn't work
            }
        }

      if (ans.length () >= 1)
        source_text = ans(0).string_value ();
      if (source_text.size ())
        got_source_text = true;

      if (!got_source_text)
        {
          octave_value_list args;
          args.append ("-q");
          args.append (fn_name);
          try
            {
              if (fn_name.size ())
                ans = interp.feval ("type", args, 1);
            }
          catch (execution_exception &)
            {
              // Didn't work
            }
        }

      if (ans.length () >= 1)
        source_text = ans(0).string_value ();
      if (source_text.size ())
        got_source_text = true;

      if (got_source_text)
        {
          // Split source by row
          vector<string> v_rows;

          std::stringstream ss(source_text);
          string buff;

          while(std::getline (ss, buff, '\n'))
              v_rows.push_back (buff);

          map_fn_to_sourcerows[fn_name] = v_rows;
        }
    }

  // Get bytecode "source code" rows
  for (auto kv : m_map_fn_stats)
    {
      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;

      auto v_ls = opcodes_to_strings (stats.m_code, stats.m_ids);

      map_fn_to_opcodes_stringrows[fn_name] = v_ls;
    }

  // Annotate bytecode
  for (auto kv : m_map_fn_stats)
    {
      std::string ans;

      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;

      auto v_ls = map_fn_to_opcodes_stringrows[fn_name];
      int64_t fn_cum_t = map_fn_to_cum_t[fn_name];

      for (auto ls : v_ls)
        {
          int ip = ls.first; // Opcode offset
          string s = ls.second; // Text representation of the opcode

          // Ignore strange data
          if (ip < 0)
            continue;

          if (static_cast<unsigned> (ip) >= stats.m_v_cum_t.size () || (stats.m_v_cum_t[ip] == 0 && stats.m_v_cum_call_t[ip] == 0))
          {
            ans += x_snprintf ("\t%*s %5d: %s\n", 43, "", ip, s.c_str ());
            continue;
          }

          int64_t n_hits = stats.m_v_n_cum[ip];
          int64_t t_op = stats.m_v_cum_t[ip] + stats.m_v_cum_call_t[ip];
          double share_of_fn = 100. * static_cast<double> (t_op) / fn_cum_t;

          // Try to make the table neat around the decimal separator
          int wholes = floor (share_of_fn);
          int rest = (share_of_fn - wholes) * 100;

          if (share_of_fn >= 0.1)
            ans += x_snprintf ("\t%8lld %12lld ns %5d.%-3d %% %12d: %s\n", static_cast<long long> (n_hits), static_cast<long long> (t_op), wholes, rest, ip, s.c_str ());
          else
            ans += x_snprintf ("\t%8lld %12lld ns  %7.3e%% %12d: %s\n", static_cast<long long> (n_hits), static_cast<long long> (t_op), share_of_fn, ip, s.c_str ());
        }

      map_fn_to_annotated_bytecode[fn_name] = ans;
    }

  // Annotate source code
  for (auto kv : m_map_fn_stats)
    {
      std::string ans;

      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;

      // First we need to create a map between opcode offset and source line
      auto v_ip_s = map_fn_to_opcodes_stringrows[fn_name];

      map<int, int> map_op_offset_to_src_line;

      for (auto ip_s : v_ip_s)
        {
          int ip = ip_s.first;
          loc_entry loc = vm::find_loc (ip, stats.m_loc_entries);
          map_op_offset_to_src_line[ip] = loc.m_line;
        }

      // Sum up the time spent on a source line
      map<int, int64_t> map_srcline_to_tcum;
      map<int, int64_t> map_srcline_to_nhits;

      for (unsigned ip = 0; ip < stats.m_v_cum_t.size (); ip++)
        {
          int64_t tcum = stats.m_v_cum_t[ip] + stats.m_v_cum_call_t[ip];
          int64_t nhits = stats.m_v_n_cum[ip];
          int src_line = map_op_offset_to_src_line[ip];
          map_srcline_to_tcum[src_line] += tcum;
          map_srcline_to_nhits[src_line] += nhits;
        }

      auto v_src_rows = map_fn_to_sourcerows[fn_name];
      // Annotate the source code

      // Put all time spent in opcodes that does not correnspond to any source line,
      // on the first row with "function.*fnname" on.
      bool found = false;
      for (unsigned i = 0; i < v_src_rows.size(); i++)
        {
          string &row = v_src_rows[i];
          std::size_t func_idx = row.find ("function");
          std::size_t name_idx = row.find (stats.m_fn_name);

          if (func_idx == string::npos || name_idx == string::npos)
            continue;

          string def = row.substr (0, func_idx + strlen ("function"));

          // Any comment making it a fake?
          if (def.find ('#') != string::npos || def.find ('%') != string::npos)
            continue;

          int line_nr = i + 1;
          map_srcline_to_tcum[line_nr] += map_srcline_to_tcum[-1];
          map_srcline_to_nhits[line_nr] += map_srcline_to_nhits[-1];
          found = true;
          break;
        }

      if (!found)
      {
        map_srcline_to_tcum[1] += map_srcline_to_tcum[-1];
        map_srcline_to_nhits[1] += map_srcline_to_nhits[-1];
      }
      int64_t fn_cum_t = map_fn_to_cum_t[fn_name];

      for (unsigned i = 0; i < v_src_rows.size(); i++)
        {
          int line_nr = i + 1;
          int64_t t_line_cum = map_srcline_to_tcum[line_nr];
          int64_t n_hits = map_srcline_to_nhits[line_nr];

          double share_of_fn = 100. * static_cast<double> (t_line_cum) / fn_cum_t;

          // Try to make the table neat around the decimal separator
          int wholes = floor (share_of_fn);
          int rest = (share_of_fn - wholes) * 100;

          string src_line = v_src_rows[i];

          if (share_of_fn == 0)
            ans += x_snprintf ("\t%*s %5d: %s\n", 43, "", line_nr, src_line.c_str ());
          else if (share_of_fn >= 0.1)
            ans += x_snprintf ("\t%8lld %12lld ns %5d.%-3d %% %12d: %s\n", static_cast<long long> (n_hits), static_cast<long long> (t_line_cum), wholes, rest, line_nr, src_line.c_str ());
          else
            ans += x_snprintf ("\t%8lld %12lld ns  %7.3e%% %12d: %s\n", static_cast<long long> (n_hits), static_cast<long long> (t_line_cum), share_of_fn, line_nr, src_line.c_str ());
        }

      map_fn_to_annotated_source[fn_name] = ans;
    }

  map<int64_t, string> map_cumt_to_fn;
  for (auto &kv : map_fn_to_cum_t)
    map_cumt_to_fn[kv.second] = kv.first;

  int64_t t_tot = 0;
  for (auto &kv : map_fn_to_cum_t)
    t_tot += kv.second;

  // Print stuff to the user

  cout << "\n\n\nProfiled functions:\n";
  cout << "\tRuntime order:\n";
  for (auto it = map_cumt_to_fn.rbegin (); it != map_cumt_to_fn.rend (); it++)
    printf ("\t\t%12lld ns %3.0f%% %s\n", static_cast<long long> (it->first), it->first * 100. / t_tot, it->second.c_str ());
  cout << "\tFirst call order:\n";
  for (string fn_name : m_fn_first_call_order)
  {
    int64_t tcum = map_fn_to_cum_t[fn_name];
    printf ("\t\t%12lld ns %3.0f%% %s\n", static_cast<long long> (tcum), tcum * 100. / t_tot, fn_name.c_str ());
  }

  for (auto kv : m_map_fn_stats)
    {
      string fn_name = kv.first;
      vm_profiler_fn_stats &stats = kv.second;

      int64_t fn_cum_t = map_fn_to_cum_t[fn_name];
      int64_t fn_self_cum_t = map_fn_to_self_cum_t[fn_name];
      string annotated_source = map_fn_to_annotated_source[fn_name];
      string annotated_bytecode = map_fn_to_annotated_bytecode[fn_name];

      cout << "\n\n\nFunction: " << kv.first << "\n\n";
      if (stats.m_fn_file.size ())
        cout << "\tFile: " << stats.m_fn_file << "\n";
      cout << "\tAmount of calls: " << static_cast<long long> (stats.m_n_calls) << "\n";
      cout << "\tCallers:         ";
      for (string caller : stats.m_set_callers)
        cout << caller << " ";
      cout << "\n";
      printf ("\tCumulative time: %9.5gs %lld ns\n", fn_cum_t/1e9, static_cast<long long> (fn_cum_t));
      printf ("\tCumulative self time: %9.5gs %lld ns\n", fn_self_cum_t/1e9, static_cast<long long> (fn_self_cum_t));
      cout << "\n\n";

      if (annotated_source.size ())
      {
         cout << "\tAnnotated source:\n";
         cout << "\t     ops         time       share\n";
         cout << "\n";
         cout << annotated_source << "\n\n";
      }
      if (annotated_bytecode.size ())
      {
        cout << "\tAnnotated bytecode:\n";
        cout << "\t     hits         time       share\n";
        cout << "\n";
        cout << annotated_bytecode << "\n\n";
      }
      cout << "\n";
    }

  cout << std::flush;
}

void
vm_profiler::enter_fn (std::string caller_name, bytecode &bc)
{
  unsigned char *code = bc.m_code.data ();
  std::string *name_data = bc.m_ids.data ();
  unwind_data *unwind_data = &bc.m_unwind_data;

  std::string callee_name = bc.m_data[2].string_value (); // profiler_name () querried at compile time

  enter_fn (callee_name, caller_name, unwind_data, name_data, code);
}

void
vm_profiler::enter_fn (std::string fn_name, std::string caller, octave::unwind_data *unwind_data, std::string *name_data, unsigned char *code)
{
  if (!m_map_fn_stats.count (fn_name))
    m_fn_first_call_order.push_back (fn_name);

  vm_profiler_fn_stats &callee_stat = m_map_fn_stats[fn_name];

  callee_stat.m_set_callers.insert (caller);
  callee_stat.m_v_callers.push_back (caller);
  callee_stat.m_n_calls++;

  vm_profiler_call call{};
  call.m_callee = fn_name;
  call.m_caller = caller;

  int64_t now = unow ();
  call.m_entry_time = now;

  m_shadow_call_stack.push_back (call);

  callee_stat.m_v_t.push_back (now);
  callee_stat.m_v_ip.push_back (0);

  if (callee_stat.m_code.size ())
    return;

  callee_stat.m_fn_file = unwind_data->m_file;
  callee_stat.m_fn_name = unwind_data->m_name;

  // We need to copy the bytecode with id names to the stat object to be able
  // to print it later.
  unsigned n_code = unwind_data->m_code_size;
  unsigned n_ids = unwind_data->m_ids_size;
  callee_stat.m_code = std::vector<unsigned char> (n_code);
  callee_stat.m_ids = std::vector<std::string> (n_ids);

  callee_stat.m_loc_entries = unwind_data->m_loc_entry;

  for (unsigned i = 0; i < n_code; i++)
    callee_stat.m_code[i] = code[i];
  for (unsigned i = 0; i < n_ids; i++)
    callee_stat.m_ids[i] = name_data[i];
}

void
vm_profiler::purge_shadow_stack ()
{
  warning ("profiler shadow stack got messed up. Measurement results might be inaccurate");

  m_shadow_call_stack.clear ();

  for (auto &kv : m_map_fn_stats)
  {
    auto &v = kv.second;
    v.m_v_callers.clear ();
    v.m_v_t.clear ();
    v.m_v_ip.clear ();
  }
}

void
vm_profiler::exit_fn (std::string fn_name)
{
  {
    int64_t t_exit = unow ();

    vm_profiler_fn_stats &callee_stat = m_map_fn_stats[fn_name];

    // Add the cost of the RET up till now to the callee
    if (callee_stat.m_v_t.size () && callee_stat.m_v_t.back () != -1)
      {
        int64_t t0 = callee_stat.m_v_t.back ();
        int64_t dt = t_exit - t0;

        callee_stat.add_t (dt);
        this->add_t (dt);
      }

    if (!m_shadow_call_stack.size ())
      goto error;
    if (!callee_stat.m_v_callers.size ())
      goto error;

    bool is_recursive = false;
    for (auto &call : m_shadow_call_stack)
      {
        if (call.m_caller == fn_name)
          {
            is_recursive = true;
            break;
          }
      }

    vm_profiler_call call = m_shadow_call_stack.back ();
    m_shadow_call_stack.pop_back ();

    std::string caller = call.m_caller;

    std::string caller_according_to_callee = callee_stat.m_v_callers.back ();

    // Pop one level
    callee_stat.m_v_callers.pop_back ();
    callee_stat.m_v_t.pop_back ();
    callee_stat.m_v_ip.pop_back ();

    if (caller_according_to_callee != caller)
      goto error;

    if (caller != "") // If the caller name is "" the callee has no profiled caller
      {
        vm_profiler_fn_stats &caller_stat = m_map_fn_stats[caller];

        if (!caller_stat.m_v_t.size ())
          goto error;

        int64_t caller_enters_call = caller_stat.m_v_t.back ();
        int64_t caller_enters_callee = call.m_entry_time;
        int64_t caller_call_overhead = caller_enters_callee - caller_enters_call;
        int64_t callee_dt = call.m_t_self_cum + call.m_t_call_cum - caller_call_overhead;

        // Add the call's cumulative time to the caller's "time spent in bytecode call"-vector
        // unless the call is recursive (to prevent confusing double book keeping of the time).
        unsigned caller_ip = caller_stat.m_v_ip.back ();
        caller_stat.maybe_resize (caller_ip);

        if (!is_recursive)
        {
          // Add to cumulative spent in call from this ip, in caller
          caller_stat.m_v_cum_call_t[caller_ip] += callee_dt;
          // Add to cumulative time spent in *the latest call* to caller
          if (m_shadow_call_stack.size ())
            m_shadow_call_stack.back ().m_t_call_cum += callee_dt;
        }
        // Change the caller's last timestamp to now and subtract the caller's call overhead.
        caller_stat.m_v_t.back () = unow () - caller_call_overhead;
      }
    return;
  }
error:
  purge_shadow_stack ();
  return;
}

void
vm::output_ignore_data::push_frame (vm &vm)
{
  vm.m_tw->set_auto_fcn_var (stack_frame::IGNORED, m_ov_pending_ignore_matrix);
  m_ov_pending_ignore_matrix = {}; // Clear ignore matrix so that the next call wont ignore anything
  m_v_lvalue_list.push_back (vm.m_tw->lvalue_list ()); // Will be restored in output_ignore_data::pop_frame ()
  m_v_owns_lvalue_list.push_back (false); // Caller owns the current lvalue

  vm.m_tw->set_lvalue_list (nullptr); // There is not lvalue list set for the new frame
}

void
vm::output_ignore_data::clear_ignore (vm &vm)
{
  CHECK_PANIC (m_v_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size () == m_v_lvalue_list.size ());

  // If the output_ignore_data object owns the current lvalue list
  // we need to free it.
  auto *current_lval_list = vm.m_tw->lvalue_list ();

  bool owns_lval_list = m_v_owns_lvalue_list.back ();
  m_v_owns_lvalue_list.back () = false;

  if (owns_lval_list)
    delete current_lval_list;

  // Restore the prior lvalue list in the tree walker
  vm.m_tw->set_lvalue_list (m_v_lvalue_list.back ());
  m_v_lvalue_list.back () = nullptr;

  m_ov_pending_ignore_matrix = {};
}

void
vm::output_ignore_data::pop_frame (vm &vm)
{
  CHECK_PANIC (m_v_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size () == m_v_lvalue_list.size ());

  // If the output_ignore_data object owns the current lvalue list
  // we need to free it.
  auto *current_lval_list = vm.m_tw->lvalue_list ();

  bool owns_lval_list = m_v_owns_lvalue_list.back ();
  m_v_owns_lvalue_list.pop_back ();

  if (owns_lval_list)
    delete current_lval_list;

  // Restore the prior lvalue list in the tree walker
  vm.m_tw->set_lvalue_list (m_v_lvalue_list.back ());
  m_v_lvalue_list.pop_back ();
}

void
vm::output_ignore_data::set_ignore_anon (vm &vm, octave_value ignore_matrix)
{
  CHECK_PANIC (m_ov_pending_ignore_matrix.is_nil ());
  CHECK_PANIC (m_v_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size () == m_v_lvalue_list.size ());

  // For anonymous functions we propagate the current ignore matrix and lvalue list to the callee.

  m_ov_pending_ignore_matrix = ignore_matrix;
  // Since the caller owns the lvalue list, we need to note not to delete the lvalue list when popping
  // the callee frame.
  vm.m_tw->set_lvalue_list (m_v_lvalue_list.back ());
}

void
vm::output_ignore_data::set_ignore (vm &vm, octave_value ignore_matrix,
                                    std::list<octave_lvalue> *new_lval_list)
{
  CHECK_PANIC (m_ov_pending_ignore_matrix.is_nil ());
  CHECK_PANIC (m_v_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size ());
  CHECK_PANIC (m_v_owns_lvalue_list.size () == m_v_lvalue_list.size ());

  m_ov_pending_ignore_matrix = ignore_matrix;
  m_v_owns_lvalue_list.back () = true;
  m_v_lvalue_list.back () = vm.m_tw->lvalue_list ();
  vm.m_tw->set_lvalue_list (new_lval_list);
}

bool
vm::maybe_compile_or_compiled (octave_user_code *fn, stack_frame::local_vars_map *locals)
{
  if (!fn)
    return false;

  if (fn->is_compiled ())
    return true;

  if (V__vm_enable__ && !fn->compilation_failed ())
    {
      try
        {
          if (fn->is_anonymous_function ())
            {
              CHECK_PANIC (locals);
              octave::compile_anon_user_function (*fn, false, *locals);
            }
          else
            octave::compile_user_function (*fn, false);

          return true;
        }
      catch (std::exception &e)
        {
          warning_with_id ("Octave:bytecode-compilation",
                           "auto-compilation of %s failed with message %s",
                           fn->name().c_str (), e.what ());
          return false;
        }
    }

  return false;
}

octave_value_list
vm::call (tree_evaluator& tw, int nargout, const octave_value_list& xargs,
          octave_user_code *fn, std::shared_ptr<stack_frame> context)
{
  // If number of outputs unknown, pass nargout=1 to the function being called
  if (nargout < 0)
    nargout = 1;

  CHECK_PANIC (fn);
  CHECK_PANIC (fn->is_compiled ());

  bool call_script = fn->is_user_script ();

  if (call_script && (xargs.length () != 0 || nargout != 0))
    error ("invalid call to script %s", fn->name ().c_str ());

  if (tw.m_call_stack.size () >= static_cast<std::size_t> (tw.m_max_recursion_depth))
    error ("max_recursion_depth exceeded");

  octave_value_list args (xargs);

  bytecode &bc = fn->get_bytecode ();

  vm vm (&tw, bc);

  bool caller_is_bytecode = tw.get_current_stack_frame ()->is_bytecode_fcn_frame ();

  // Pushes a bytecode stackframe. nargin is set inside the VM.
  if (context)
    tw.push_stack_frame (vm, fn, nargout, 0, context); // Closure context for nested frames
  else
    tw.push_stack_frame (vm, fn, nargout, 0);

  // The arg names of root stackframe in VM need to be set here, unless the caller is bytecode.
  // The caller can be bytecode if evalin("caller", ...) is used in some uncompiled function.
  if (!caller_is_bytecode)
    tw.set_auto_fcn_var (stack_frame::ARG_NAMES, Cell (xargs.name_tags ()));
  if (!call_script)
    {
      Matrix ignored_outputs = tw.ignored_fcn_outputs ();
      if (ignored_outputs.numel())
        {
          vm.caller_ignores_output ();
          tw.set_auto_fcn_var (stack_frame::IGNORED, ignored_outputs);
        }
    }

  octave_value_list ret;

  try {
    ret = vm.execute_code (args, nargout);
  } catch (std::exception &e) {
    if (vm.m_dbg_proper_return == false)
      {
        std::cout << e.what () << std::endl;
        // TODO: Replace with panic when the VM almost works

        // Some test code eats errors messages, so we print to stderr too.
        std::cerr << "VM error " << __LINE__ << ": Exception in " << fn->name () << " escaped the VM\n";
        error("VM error %d: " "Exception in %s escaped the VM\n", __LINE__, fn->name ().c_str());
      }

    tw.pop_stack_frame ();
    throw;
  } catch (const quit_debug_exception &qde) {
    if (vm.m_dbg_proper_return == false)
      panic ("quit debug exception escaping the vm");

    tw.pop_stack_frame ();
    throw;
  }


  tw.pop_stack_frame ();
  return ret;
}

// Debugging functions to be called from gdb

void
vm_debug_print_obv (octave_base_value *obv)
{
  obv->print (std::cout);
}

void
vm_debug_print_ov (octave_value ov)
{
  ov.print (std::cout);
}

void
vm_debug_print_ovl (octave_value_list ovl)
{
  for (int i = 0; i < ovl.length (); i++)
    {
      ovl (i).print (std::cout);
    }
}


extern "C" void dummy_mark_1 (void)
{
  asm ("");
}

extern "C" void dummy_mark_2 (void)
{
  asm ("");
}

#endif