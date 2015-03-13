/*=========================================================================

  Program:   Insight Segmentation & Registration Toolkit
  Module:    $RCSfile: SimpleWarpImageFilter.h,v $
  Language:  C++
  Date:      $Date: 2009-10-29 11:19:00 $
  Version:   $Revision: 1.31 $

  Copyright (c) Insight Software Consortium. All rights reserved.
  See ITKCopyright.txt or http://www.itk.org/HTML/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even 
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#ifndef __MultiImageAffineMSDMetricFilter_h
#define __MultiImageAffineMSDMetricFilter_h

#include "itkImageBase.h"
#include "itkImageToImageFilter.h"
#include "itkPoint.h"
#include "itkVectorImage.h"
#include "itkMatrixOffsetTransformBase.h"

/**
 * Flatten an affine transform to a flat array
 */
template<class TFloat, class TFloatArr, unsigned int VDim>
static void flatten_affine_transform(
    const itk::MatrixOffsetTransformBase<TFloat, VDim, VDim> *transform,
    TFloatArr *flat_array)
{
  int pos = 0;
  for(int i = 0; i < VDim; i++)
    {
    flat_array[pos++] = transform->GetOffset()[i];
    for(int j = 0; j < VDim; j++)
      flat_array[pos++] = transform->GetMatrix()(i,j);
    }
}

/**
 * Unflatten a flat array to an affine transform
 */
template<class TFloat, class TFloatArr, unsigned int VDim>
static void unflatten_affine_transform(
   const TFloatArr *flat_array,
   itk::MatrixOffsetTransformBase<TFloat, VDim, VDim> *transform,
   double scaling = 1.0)
{
  typename itk::MatrixOffsetTransformBase<TFloat, VDim, VDim>::MatrixType matrix;
  typename itk::MatrixOffsetTransformBase<TFloat, VDim, VDim>::OffsetType offset;

  int pos = 0;
  for(int i = 0; i < VDim; i++)
    {
    offset[i] = flat_array[pos++] * scaling;
    for(int j = 0; j < VDim; j++)
      matrix(i, j) = flat_array[pos++] * scaling;
    }

  transform->SetMatrix(matrix);
  transform->SetOffset(offset);
}


/**
 * This filter computes the gradient of the affine transform. The inputs to this filter
 * should come from a metric
 */
template <class TMetricTraits>
class ITK_EXPORT MultiImageAffineMetricFilter :
    public itk::ImageToImageFilter<typename TMetricTraits::MetricImageType,
                                   typename TMetricTraits::MetricImageType>
{
public:

  /** Type definitions from the traits class */
  typedef typename TMetricTraits::MetricImageType       InputImageType;
  typedef typename TMetricTraits::GradientImageType     GradientImageType;

  /** Standard class typedefs. */
  typedef MultiImageAffineMetricFilter                              Self;
  typedef itk::ImageToImageFilter<InputImageType,InputImageType>    Superclass;
  typedef itk::SmartPointer<Self>                                   Pointer;
  typedef itk::SmartPointer<const Self>                             ConstPointer;

  /** Method for creation through the object factory. */
  itkNewMacro(Self)

  /** Run-time type information (and related methods) */
  itkTypeMacro( MultiImageAffineMetricFilter, ImageToImageFilter )

  /** Determine the image dimension. */
  itkStaticConstMacro(ImageDimension, unsigned int, InputImageType::ImageDimension );

  /** Typedef to describe the output image region type. */

  /** Inherit some types from the superclass. */
  typedef itk::ImageBase<ImageDimension>              ImageBaseType;
  typedef typename InputImageType::PixelType          InputPixelType;
  typedef typename GradientImageType::PixelType       GradientPixelType;
  typedef typename InputImageType::IndexType          IndexType;
  typedef typename InputImageType::IndexValueType     IndexValueType;
  typedef typename InputImageType::SizeType           SizeType;
  typedef typename InputImageType::SpacingType        SpacingType;
  typedef typename InputImageType::DirectionType      DirectionType;
  typedef typename InputImageType::RegionType         OutputImageRegionType;

  /** Information from the parent class */
  typedef itk::MatrixOffsetTransformBase<double, ImageDimension, ImageDimension> TransformType;
  typedef typename TransformType::Pointer             TransformPointer;

  /** Set the metric image, which is passed through as the output */
  void SetMetricImage(InputImageType *metric)
    { this->itk::ProcessObject::SetInput("Primary", metric); }

  InputImageType *GetMetricImage()
    { return dynamic_cast<InputImageType *>(this->itk::ProcessObject::GetInput("Primary")); }

  /** Set the metric image, which is passed through as the output */
  void SetGradientImage(GradientImageType *metric)
    { this->itk::ProcessObject::SetInput("gradient", metric); }

  GradientImageType *GetGradientImage()
    { return dynamic_cast<GradientImageType *>(this->itk::ProcessObject::GetInput("gradient")); }

  /** Set the metric image, which is passed through as the output */
  void SetMovingDomainMaskImage(InputImageType *metric)
    { this->itk::ProcessObject::SetInput("moving_mask", metric); }

  InputImageType *GetMovingDomainMaskImage()
    { return dynamic_cast<InputImageType *>(this->itk::ProcessObject::GetInput("moving_mask")); }

  /** Set the metric image, which is passed through as the output */
  void SetMovingDomainMaskGradientImage(GradientImageType *metric)
    { this->itk::ProcessObject::SetInput("moving_mask_gradient", metric); }

  GradientImageType *GetMovingDomainMaskGradientImage()
    { return dynamic_cast<GradientImageType *>(this->itk::ProcessObject::GetInput("moving_mask_gradient")); }

  /** Whether the gradient is computed */
  itkSetMacro(ComputeGradient, bool)
  itkGetMacro(ComputeGradient, bool)

  /** Set the transform field. */
  void SetTransform(TransformType *transform)
    { m_Transform = transform; }

  itkGetConstMacro(Transform, TransformType *)

  /** The gradient (in the form of a transform) after running the filter */
  itkGetConstMacro(MetricGradient, TransformType *)

  /** Get the computed metric value */
  itkGetMacro(MetricValue, double)

protected:
  MultiImageAffineMetricFilter() : m_ComputeGradient(false) {}
  ~MultiImageAffineMetricFilter() {}

  void PrintSelf(std::ostream& os, itk::Indent indent) const
    { this->PrintSelf(os, indent); }

  /** SimpleWarpImageFilter is implemented as a multi-threaded filter.
   * As such, it needs to provide and implementation for
   * ThreadedGenerateData(). */
  void ThreadedGenerateData(const OutputImageRegionType& outputRegionForThread,
                            itk::ThreadIdType threadId );

  /** It is difficult to compute in advance the input image region
   * required to compute the requested output region. Thus the safest
   * thing to do is to request for the whole input image.
   *
   * For the deformation field, the input requested region
   * set to be the same as that of the output requested region. */
  virtual void GenerateInputRequestedRegion();

  /** Override since input passed to output */
  virtual void EnlargeOutputRequestedRegion(itk::DataObject *data);

  /** This method is used to set the state of the filter before
   * multi-threading. */
  virtual void BeforeThreadedGenerateData();

  /** This method is used to set the state of the filter after
   * multi-threading. */
  virtual void AfterThreadedGenerateData();

  /** Allocate outputs - just pass through the input */
  virtual void AllocateOutputs();

  void VerifyInputInformation() {}

  // Object to assist specializaiton
  struct DispatchBase {};
  template <unsigned int VDim> struct Dispatch : public DispatchBase {};

private:
  MultiImageAffineMetricFilter(const Self&); //purposely not implemented
  void operator=(const Self&); //purposely not implemented

  // Transform pointer
  TransformPointer                m_Transform;

  // Data accumulated for each thread
  struct ThreadData {
    double metric, mask;
    vnl_vector<double> gradient, grad_mask;
    ThreadData() : metric(0.0), mask(0.0),
      gradient(ImageDimension * (ImageDimension+1), 0.0),
      grad_mask(ImageDimension * (ImageDimension+1), 0.0) {}
  };

  std::vector<ThreadData>         m_ThreadData;

  // Gradient
  TransformPointer                m_MetricGradient;

  // Metric scaled by the mask
  double                          m_MetricValue;

  // Whether the gradient is computed
  bool                            m_ComputeGradient;
};






#ifndef ITK_MANUAL_INSTANTIATION
#include "MultiImageAffineMSDMetricFilter.txx"
#endif

#endif
