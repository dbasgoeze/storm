// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2009-2010 Gael Guennebaud <gael.guennebaud@inria.fr>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef STORMEIGEN_ARRAYWRAPPER_H
#define STORMEIGEN_ARRAYWRAPPER_H

namespace StormEigen { 

/** \class ArrayWrapper
  * \ingroup Core_Module
  *
  * \brief Expression of a mathematical vector or matrix as an array object
  *
  * This class is the return type of MatrixBase::array(), and most of the time
  * this is the only way it is use.
  *
  * \sa MatrixBase::array(), class MatrixWrapper
  */

namespace internal {
template<typename ExpressionType>
struct traits<ArrayWrapper<ExpressionType> >
  : public traits<typename remove_all<typename ExpressionType::Nested>::type >
{
  typedef ArrayXpr XprKind;
  // Let's remove NestByRefBit
  enum {
    Flags0 = traits<typename remove_all<typename ExpressionType::Nested>::type >::Flags,
    Flags = Flags0 & ~NestByRefBit
  };
};
}

template<typename ExpressionType>
class ArrayWrapper : public ArrayBase<ArrayWrapper<ExpressionType> >
{
  public:
    typedef ArrayBase<ArrayWrapper> Base;
    STORMEIGEN_DENSE_PUBLIC_INTERFACE(ArrayWrapper)
    STORMEIGEN_INHERIT_ASSIGNMENT_OPERATORS(ArrayWrapper)
    typedef typename internal::remove_all<ExpressionType>::type NestedExpression;

    typedef typename internal::conditional<
                       internal::is_lvalue<ExpressionType>::value,
                       Scalar,
                       const Scalar
                     >::type ScalarWithConstIfNotLvalue;

    typedef typename internal::ref_selector<ExpressionType>::type NestedExpressionType;

    STORMEIGEN_DEVICE_FUNC
    explicit STORMEIGEN_STRONG_INLINE ArrayWrapper(ExpressionType& matrix) : m_expression(matrix) {}

    STORMEIGEN_DEVICE_FUNC
    inline Index rows() const { return m_expression.rows(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index cols() const { return m_expression.cols(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index outerStride() const { return m_expression.outerStride(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index innerStride() const { return m_expression.innerStride(); }

    STORMEIGEN_DEVICE_FUNC
    inline ScalarWithConstIfNotLvalue* data() { return m_expression.const_cast_derived().data(); }
    STORMEIGEN_DEVICE_FUNC
    inline const Scalar* data() const { return m_expression.data(); }

    STORMEIGEN_DEVICE_FUNC
    inline CoeffReturnType coeff(Index rowId, Index colId) const
    {
      return m_expression.coeff(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline Scalar& coeffRef(Index rowId, Index colId)
    {
      return m_expression.const_cast_derived().coeffRef(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline const Scalar& coeffRef(Index rowId, Index colId) const
    {
      return m_expression.const_cast_derived().coeffRef(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline CoeffReturnType coeff(Index index) const
    {
      return m_expression.coeff(index);
    }

    STORMEIGEN_DEVICE_FUNC
    inline Scalar& coeffRef(Index index)
    {
      return m_expression.const_cast_derived().coeffRef(index);
    }

    STORMEIGEN_DEVICE_FUNC
    inline const Scalar& coeffRef(Index index) const
    {
      return m_expression.const_cast_derived().coeffRef(index);
    }

    template<int LoadMode>
    inline const PacketScalar packet(Index rowId, Index colId) const
    {
      return m_expression.template packet<LoadMode>(rowId, colId);
    }

    template<int LoadMode>
    inline void writePacket(Index rowId, Index colId, const PacketScalar& val)
    {
      m_expression.const_cast_derived().template writePacket<LoadMode>(rowId, colId, val);
    }

    template<int LoadMode>
    inline const PacketScalar packet(Index index) const
    {
      return m_expression.template packet<LoadMode>(index);
    }

    template<int LoadMode>
    inline void writePacket(Index index, const PacketScalar& val)
    {
      m_expression.const_cast_derived().template writePacket<LoadMode>(index, val);
    }

    template<typename Dest>
    STORMEIGEN_DEVICE_FUNC
    inline void evalTo(Dest& dst) const { dst = m_expression; }

    const typename internal::remove_all<NestedExpressionType>::type& 
    STORMEIGEN_DEVICE_FUNC
    nestedExpression() const 
    {
      return m_expression;
    }

    /** Forwards the resizing request to the nested expression
      * \sa DenseBase::resize(Index)  */
    STORMEIGEN_DEVICE_FUNC
    void resize(Index newSize) { m_expression.const_cast_derived().resize(newSize); }
    /** Forwards the resizing request to the nested expression
      * \sa DenseBase::resize(Index,Index)*/
    STORMEIGEN_DEVICE_FUNC
    void resize(Index rows, Index cols) { m_expression.const_cast_derived().resize(rows,cols); }

  protected:
    NestedExpressionType m_expression;
};

/** \class MatrixWrapper
  * \ingroup Core_Module
  *
  * \brief Expression of an array as a mathematical vector or matrix
  *
  * This class is the return type of ArrayBase::matrix(), and most of the time
  * this is the only way it is use.
  *
  * \sa MatrixBase::matrix(), class ArrayWrapper
  */

namespace internal {
template<typename ExpressionType>
struct traits<MatrixWrapper<ExpressionType> >
 : public traits<typename remove_all<typename ExpressionType::Nested>::type >
{
  typedef MatrixXpr XprKind;
  // Let's remove NestByRefBit
  enum {
    Flags0 = traits<typename remove_all<typename ExpressionType::Nested>::type >::Flags,
    Flags = Flags0 & ~NestByRefBit
  };
};
}

template<typename ExpressionType>
class MatrixWrapper : public MatrixBase<MatrixWrapper<ExpressionType> >
{
  public:
    typedef MatrixBase<MatrixWrapper<ExpressionType> > Base;
    STORMEIGEN_DENSE_PUBLIC_INTERFACE(MatrixWrapper)
    STORMEIGEN_INHERIT_ASSIGNMENT_OPERATORS(MatrixWrapper)
    typedef typename internal::remove_all<ExpressionType>::type NestedExpression;

    typedef typename internal::conditional<
                       internal::is_lvalue<ExpressionType>::value,
                       Scalar,
                       const Scalar
                     >::type ScalarWithConstIfNotLvalue;

    typedef typename internal::ref_selector<ExpressionType>::type NestedExpressionType;

    STORMEIGEN_DEVICE_FUNC
    explicit inline MatrixWrapper(ExpressionType& matrix) : m_expression(matrix) {}

    STORMEIGEN_DEVICE_FUNC
    inline Index rows() const { return m_expression.rows(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index cols() const { return m_expression.cols(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index outerStride() const { return m_expression.outerStride(); }
    STORMEIGEN_DEVICE_FUNC
    inline Index innerStride() const { return m_expression.innerStride(); }

    STORMEIGEN_DEVICE_FUNC
    inline ScalarWithConstIfNotLvalue* data() { return m_expression.const_cast_derived().data(); }
    STORMEIGEN_DEVICE_FUNC
    inline const Scalar* data() const { return m_expression.data(); }

    STORMEIGEN_DEVICE_FUNC
    inline CoeffReturnType coeff(Index rowId, Index colId) const
    {
      return m_expression.coeff(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline Scalar& coeffRef(Index rowId, Index colId)
    {
      return m_expression.const_cast_derived().coeffRef(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline const Scalar& coeffRef(Index rowId, Index colId) const
    {
      return m_expression.derived().coeffRef(rowId, colId);
    }

    STORMEIGEN_DEVICE_FUNC
    inline CoeffReturnType coeff(Index index) const
    {
      return m_expression.coeff(index);
    }

    STORMEIGEN_DEVICE_FUNC
    inline Scalar& coeffRef(Index index)
    {
      return m_expression.const_cast_derived().coeffRef(index);
    }

    STORMEIGEN_DEVICE_FUNC
    inline const Scalar& coeffRef(Index index) const
    {
      return m_expression.const_cast_derived().coeffRef(index);
    }

    template<int LoadMode>
    inline const PacketScalar packet(Index rowId, Index colId) const
    {
      return m_expression.template packet<LoadMode>(rowId, colId);
    }

    template<int LoadMode>
    inline void writePacket(Index rowId, Index colId, const PacketScalar& val)
    {
      m_expression.const_cast_derived().template writePacket<LoadMode>(rowId, colId, val);
    }

    template<int LoadMode>
    inline const PacketScalar packet(Index index) const
    {
      return m_expression.template packet<LoadMode>(index);
    }

    template<int LoadMode>
    inline void writePacket(Index index, const PacketScalar& val)
    {
      m_expression.const_cast_derived().template writePacket<LoadMode>(index, val);
    }

    STORMEIGEN_DEVICE_FUNC
    const typename internal::remove_all<NestedExpressionType>::type& 
    nestedExpression() const 
    {
      return m_expression;
    }

    /** Forwards the resizing request to the nested expression
      * \sa DenseBase::resize(Index)  */
    STORMEIGEN_DEVICE_FUNC
    void resize(Index newSize) { m_expression.const_cast_derived().resize(newSize); }
    /** Forwards the resizing request to the nested expression
      * \sa DenseBase::resize(Index,Index)*/
    STORMEIGEN_DEVICE_FUNC
    void resize(Index rows, Index cols) { m_expression.const_cast_derived().resize(rows,cols); }

  protected:
    NestedExpressionType m_expression;
};

} // end namespace StormEigen

#endif // STORMEIGEN_ARRAYWRAPPER_H