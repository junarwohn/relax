/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/contrib/msc/core/printer/print_utils.h
 * \brief Common utilities for print.
 */
#ifndef TVM_CONTRIB_MSC_CORE_PRINTER_PRINT_UTILS_H_
#define TVM_CONTRIB_MSC_CORE_PRINTER_PRINT_UTILS_H_

#include <tvm/script/printer/doc.h>

#include <vector>

#include "msc_doc.h"

namespace tvm {
namespace contrib {
namespace msc {

using namespace tvm::script::printer;

/*!
 * \brief Utils for Doc.
 */
class DocUtils {
 public:
  /*!
   * \brief Change object to Doc.
   * \return The Doc.
   */
  TVM_DLL static const ExprDoc ToDoc(int val);
  TVM_DLL static const ExprDoc ToDoc(int64_t val);
  TVM_DLL static const ExprDoc ToDoc(size_t val);
  TVM_DLL static const ExprDoc ToDoc(const IntImm& val);
  TVM_DLL static const ExprDoc ToDoc(const Integer& val);
  TVM_DLL static const ExprDoc ToDoc(float val);
  TVM_DLL static const ExprDoc ToDoc(double val);
  TVM_DLL static const ExprDoc ToDoc(const FloatImm& val);
  TVM_DLL static const ExprDoc ToDoc(const char* val);
  TVM_DLL static const ExprDoc ToDoc(const String& val);
  TVM_DLL static const ExprDoc ToDoc(bool val);
  TVM_DLL static const ExprDoc ToDoc(const ExprDoc& val);
  TVM_DLL static const ExprDoc ToStrDoc(const String& val);
  TVM_DLL static const PointerDoc ToPtrDoc(const String& val);

  /*!
   * \brief Change object to DeclareDoc.
   * \return The DeclareDoc.
   */
  TVM_DLL static const DeclareDoc ToDeclareDoc(const String& type, const String& variable,
                                               size_t len = 0, bool use_constructor = true);

  /*!
   * \brief Change object to AttrAccessDoc.
   * \return The AttrAccessDoc.
   */
  TVM_DLL static const AttrAccessDoc ToAttrAccessDoc(const String& value, const String& name);

  /*!
   * \brief Change object to List of Docs.
   * \return The List of Docs.
   */
  template <typename T>
  TVM_DLL static const Array<ExprDoc> ToDocList(const std::vector<T>& values) {
    Array<ExprDoc> elements;
    for (const auto& v : values) {
      elements.push_back(ToDoc(v));
    }
    return elements;
  }
  template <typename T>
  TVM_DLL static const Array<ExprDoc> ToDocList(const Array<T>& values) {
    Array<ExprDoc> elements;
    for (const auto& v : values) {
      elements.push_back(ToDoc(v));
    }
    return elements;
  }

  /*!
   * \brief Change object to ListDoc.
   * \return The ListDoc.
   */
  template <typename T>
  TVM_DLL static const StrictListDoc ToListDoc(const std::vector<T>& values,
                                               bool allow_empty = false) {
    if (values.size() > 0 || allow_empty) {
      return StrictListDoc(ListDoc(ToDocList(values)), allow_empty);
    }
    return StrictListDoc(ListDoc(), false);
  }
  template <typename T>
  TVM_DLL static const StrictListDoc ToListDoc(const Array<T>& values, bool allow_empty = false) {
    if (values.size() > 0 || allow_empty) {
      return StrictListDoc(ListDoc(ToDocList(values)), allow_empty);
    }
    return StrictListDoc(ListDoc(), false);
  }

  /*!
   * \brief Change object to IndexDoc.
   * \return The ListDoc.
   */
  template <typename T>
  TVM_DLL static const IndexDoc ToIndexDoc(const String& value, const std::vector<T>& indices) {
    Array<Doc> doc_indices;
    for (const auto& i : indices) {
      doc_indices.push_back(ToDoc(i));
    }
    return IndexDoc(IdDoc(value), doc_indices);
  }
  template <typename T>
  TVM_DLL static const IndexDoc ToIndexDoc(const String& value, const Array<T>& indices) {
    Array<Doc> doc_indices;
    for (const auto& i : indices) {
      doc_indices.push_back(ToDoc(i));
    }
    return IndexDoc(IdDoc(value), doc_indices);
  }

  /*!
   * \brief Change object to AssignDoc.
   * \return The AssignDoc.
   */
  template <typename T>
  TVM_DLL static const AssignDoc ToAssignDoc(const String& lhs, const T& rhs,
                                             const String& annotation = "") {
    if (annotation.size() == 0) {
      return AssignDoc(IdDoc(lhs), ToDoc(rhs), NullOpt);
    }
    return AssignDoc(IdDoc(lhs), ToDoc(rhs), IdDoc(annotation));
  }

  /*!
   * \brief Convert the docs to Stmts.
   * \return The Stmts.
   */
  TVM_DLL static const Array<StmtDoc> ToStmts(const Array<Doc>& docs);

  /*!
   * \brief Convert the docs to StmtBlock.
   * \return The StmtBlockDoc.
   */
  TVM_DLL static const StmtBlockDoc ToStmtBlock(const Array<Doc>& docs);
};

}  // namespace msc
}  // namespace contrib
}  // namespace tvm
#endif  // TVM_CONTRIB_MSC_CORE_PRINTER_PRINT_UTILS_H_
