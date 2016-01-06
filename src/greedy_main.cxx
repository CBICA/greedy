#include <iostream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cerrno>

#include "lddmm_common.h"
#include "lddmm_data.h"

#include <itkImageFileReader.h>
#include <itkGaussianInterpolateImageFunction.h>
#include <itkResampleImageFilter.h>
#include <itkIdentityTransform.h>
#include <itkShrinkImageFilter.h>
#include <itkAffineTransform.h>
#include <itkTransformFactory.h>

#include "MultiImageRegistrationHelper.h"
#include "FastWarpCompositeImageFilter.h"
#include <vnl/vnl_cost_function.h>
#include <vnl/vnl_random.h>
#include <vnl/algo/vnl_powell.h>

// Little helper functions
template <unsigned int VDim> class array_caster
{
public:
  template <class T> static itk::Size<VDim> to_itkSize(const T &t)
  {
    itk::Size<VDim> sz;
    for(int i = 0; i < VDim; i++)
      sz[i] = t[i];
    return sz;
  }

};

/**
 * A simple exception class with string formatting
 */
class GreedyException : public std::exception
{
public:

  GreedyException(const char *format, ...)
  {
    buffer = new char[4096];
    va_list args;
    va_start (args, format);
    vsprintf (buffer,format, args);
    va_end (args);
  }

  virtual const char* what() const throw() { return buffer; }

  virtual ~GreedyException() throw() { delete buffer; }

private:

  char *buffer;

};

int usage()
{
  printf("greedy: Paul's greedy diffeomorphic registration implementation\n");
  printf("Usage: \n");
  printf("  greedy [options]\n");
  printf("Required options: \n");
  printf("  -d DIM                      : Number of image dimensions\n");
  printf("  -i fixed.nii moving.nii     : Image pair (may be repeated)\n");
  printf("  -o output.nii               : Output file\n");  
  printf("Mode specification: \n");
  printf("  -a                          : Perform affine registration and save to output (-o)\n");
  printf("  -brute radius               : Perform a brute force search around each voxel \n");
  printf("  -r [tran_spec]              : Reslice images instead of doing registration \n");
  printf("                                tran_spec is a series of warps, affine matrices\n");
  printf("Options in deformable / affine mode: \n");
  printf("  -w weight                   : weight of the next -i pair\n");
  printf("  -m metric                   : metric for the registration (SSD or NCC 3x3x3)\n");
  printf("  -e epsilon                  : step size (default = 1.0)\n");
  printf("  -n NxNxN                    : number of iterations per level of multi-res (100x100) \n");
  printf("  -threads N                  : set the number of allowed concurrent threads\n");
  printf("  -gm mask.nii                : mask for gradient computation\n");
  printf("  -it filenames               : sequence of transforms to apply to the moving image first \n");
  printf("Specific to deformable mode: \n");
  printf("  -tscale MODE                : time step behavior mode: CONST, SCALE [def], SCALEDOWN\n");
  printf("  -s sigma1 sigma2            : smoothing for the greedy update step. Must specify units,\n");
  printf("                                either `vox` or `mm`. Default: 1.732vox, 0.7071vox\n");
  printf("  -oinv image.nii             : compute and write the inverse of the warp field into image.nii\n");
  printf("  -invexp VALUE               : how many times to take the square root of the forward\n");
  printf("                                transform when computing inverse (default=2)\n");
  printf("  -wp VALUE                   : Saved warp precision (in voxels; def=0.1; 0 for no compression).\n");
  printf("Specific to affine mode: \n");
  printf("  -ia filename                : initial affine matrix for optimization (not the same as -it) \n");
  printf("Specific to reslice mode: \n");
  printf("   -rf fixed.nii              : fixed image for reslicing\n");
  printf("   -rm moving.nii output.nii  : moving/output image pair (may be repeated)\n");
  printf("   -ri interp_mode            : interpolation for the next pair (NN, LINEAR*, LABEL sigma)\n");
  printf("For developers: \n");
  printf("  -debug-deriv                : enable periodic checks of derivatives (debug) \n");
  printf("  -debug-deriv-eps            : epsilon for derivative debugging \n");
  printf("  -dump-moving                : dump moving image at each iter\n");
  printf("  -dump-freq N                : dump frequency\n");
  printf("  -powell                     : use Powell's method instead of LGBFS\n");

  return -1;
}

struct ImagePairSpec
{
  std::string fixed;
  std::string moving;
  double weight;
};

struct InterpSpec
{
  enum InterpMode { LINEAR, NEAREST, LABELWISE };

  InterpMode mode;
  double sigma;

  InterpSpec() : mode(LINEAR), sigma(1.0) {}
};

struct ResliceSpec
{
  std::string moving;
  std::string output;
  InterpSpec interp;
};

struct TransformSpec
{
  // Transform file
  std::string filename;

  // Optional exponent (-1 for inverse, 0.5 for square root)
  double exponent;
};

struct GreedyResliceParameters
{
  // For reslice mode
  std::vector<ResliceSpec> images;

  // Reference image
  std::string ref_image;

  // Chain of transforms
  std::vector<TransformSpec> transforms;
};

struct SmoothingParameters
{
  double sigma;
  bool physical_units;
};

struct GreedyParameters
{
  enum MetricType { SSD = 0, NCC, MI };
  enum TimeStepMode { CONST=0, SCALE, SCALEDOWN };
  enum Mode { GREEDY=0, AFFINE, BRUTE, RESLICE };



  std::vector<ImagePairSpec> inputs;
  std::string output;
  unsigned int dim; 

  // Reslice parameters
  GreedyResliceParameters reslice_param;

  // Registration mode
  Mode mode;

  bool flag_dump_moving, flag_debug_deriv, flag_powell;
  int dump_frequency, threads;
  double epsilon;
  double deriv_epsilon;

  // Smoothing parameters
  SmoothingParameters sigma_pre, sigma_post;

  MetricType metric;
  TimeStepMode time_step_mode;

  // Iterations per level (i.e., 40x40x100)
  std::vector<int> iter_per_level;

  std::vector<int> metric_radius;

  std::vector<int> brute_search_radius;

  // List of transforms to apply to the moving image before registration
  std::vector<TransformSpec> moving_pre_transforms;

  // Initial affine transform
  TransformSpec initial_affine;

  // Mask for gradient
  std::string gradient_mask;

  // Inverse warp
  std::string inverse_warp;
  int inverse_exponent;

  // Precision for output warps
  double warp_precision;
};


// Helper function to map from ITK coordiante space to RAS space
template<unsigned int VDim>
void
GetVoxelSpaceToNiftiSpaceTransform(itk::ImageBase<VDim> *image,
                                   vnl_matrix<double> &A,
                                   vnl_vector<double> &b)
{
  // Generate intermediate terms
  vnl_matrix<double> m_dir, m_ras_matrix;
  vnl_diag_matrix<double> m_scale, m_lps_to_ras;
  vnl_vector<double> v_origin, v_ras_offset;

  // Compute the matrix
  m_dir = image->GetDirection().GetVnlMatrix();
  m_scale.set(image->GetSpacing().GetVnlVector());
  m_lps_to_ras.set(vnl_vector<double>(VDim, 1.0));
  m_lps_to_ras[0] = -1;
  m_lps_to_ras[1] = -1;
  A = m_lps_to_ras * m_dir * m_scale;

  // Compute the vector
  v_origin = image->GetOrigin().GetVnlVector();
  b = m_lps_to_ras * v_origin;
}

template <unsigned int VDim, typename TReal = double>
class GreedyApproach
{
public:

  typedef GreedyApproach<VDim, TReal> Self;

  typedef LDDMMData<TReal, VDim> LDDMMType;
  typedef typename LDDMMType::ImageBaseType ImageBaseType;
  typedef typename LDDMMType::ImageType ImageType;
  typedef typename LDDMMType::ImagePointer ImagePointer;
  typedef typename LDDMMType::VectorImageType VectorImageType;
  typedef typename LDDMMType::VectorImagePointer VectorImagePointer;
  typedef typename LDDMMType::CompositeImageType CompositeImageType;
  typedef typename LDDMMType::CompositeImagePointer CompositeImagePointer;

  typedef MultiImageOpticalFlowHelper<TReal, VDim> OFHelperType;

  struct ImagePair {
    ImagePointer fixed, moving;
    VectorImagePointer grad_moving;
    double weight;
  };


  static int Run(GreedyParameters &param);

  static int RunDeformable(GreedyParameters &param);

  static int RunAffine(GreedyParameters &param);

  static int RunBrute(GreedyParameters &param);

  static int RunReslice(GreedyParameters &param);

protected:

  static void ReadImages(GreedyParameters &param, OFHelperType &ofhelper);

  static void ResampleImages(GreedyParameters &param,
                             const std::vector<ImagePair> &imgRaw,
                             std::vector<ImagePair> &img,
                             int level);

  static void ReadTransformChain(const std::vector<TransformSpec> &tran_chain,
                                 ImageBaseType *ref_space,
                                 VectorImagePointer &out_warp);

  static vnl_matrix<double> MapAffineToPhysicalRASSpace(
      OFHelperType &of_helper, int level,
      typename OFHelperType::LinearTransformType *tran);

  static void MapPhysicalRASSpaceToAffine(
      OFHelperType &of_helper, int level,
      vnl_matrix<double> &Qp,
      typename OFHelperType::LinearTransformType *tran);

  /** Cost function used for conjugate gradient descent */
  class AffineCostFunction : public vnl_cost_function
  {
  public:
    typedef typename OFHelperType::LinearTransformType TransformType;


    // Construct the function
    AffineCostFunction(GreedyParameters *param, int level, OFHelperType *helper);

    // Get the parameters for the specified initial transform
    vnl_vector<double> GetCoefficients(TransformType *tran)
    {
      vnl_vector<double> x_true(this->get_number_of_unknowns());
      flatten_affine_transform(tran, x_true.data_block());
      return element_product(x_true, scaling);
    }

    // Get the transform for the specificed coefficients
    void GetTransform(const vnl_vector<double> &coeff, TransformType *tran)
    {
      vnl_vector<double> x_true = element_quotient(coeff, scaling);
      unflatten_affine_transform(x_true.data_block(), tran);
    }

    // Cost function computation
    virtual void compute(vnl_vector<double> const& x, double *f, vnl_vector<double>* g);

    const vnl_vector<double> &GetScaling() { return scaling; }

  protected:


    // Data needed to compute the cost function
    GreedyParameters *m_Param;
    OFHelperType *m_OFHelper;
    int m_Level;
    vnl_vector<double> scaling;

    // Storage for the gradient of the similarity map
    VectorImagePointer m_Phi, m_GradMetric, m_GradMask;
    ImagePointer m_Metric, m_Mask;
  };
};

template <unsigned int VDim, typename TReal>
GreedyApproach<VDim, TReal>::AffineCostFunction
::AffineCostFunction(GreedyParameters *param, int level, OFHelperType *helper)
  : vnl_cost_function(VDim * (VDim + 1))
{
  // Store the data
  m_Param = param;
  m_OFHelper = helper;
  m_Level = level;

  // Set the scaling of the parameters based on image dimensions. This makes it
  // possible to set tolerances in units of voxels. The order of change in the
  // parameters is comparable to the displacement of any point inside the image
  scaling.set_size(this->get_number_of_unknowns());

  typename TransformType::MatrixType matrix;
  typename TransformType::OffsetType offset;
  for(int i = 0; i < VDim; i++)
    {
    offset[i] = 1.0;
    for(int j = 0; j < VDim; j++)
      matrix(i, j) = helper->GetReferenceSpace(level)->GetBufferedRegion().GetSize()[j];
    }

  typename TransformType::Pointer transform = TransformType::New();
  transform->SetMatrix(matrix);
  transform->SetOffset(offset);
  flatten_affine_transform(transform.GetPointer(), scaling.data_block());

  // Allocate the working images
  m_Phi = VectorImageType::New();
  m_Phi->CopyInformation(helper->GetReferenceSpace(level));
  m_Phi->SetRegions(helper->GetReferenceSpace(level)->GetBufferedRegion());
  m_Phi->Allocate();

  m_GradMetric = VectorImageType::New();
  m_GradMetric->CopyInformation(helper->GetReferenceSpace(level));
  m_GradMetric->SetRegions(helper->GetReferenceSpace(level)->GetBufferedRegion());
  m_GradMetric->Allocate();

  m_GradMask = VectorImageType::New();
  m_GradMask->CopyInformation(helper->GetReferenceSpace(level));
  m_GradMask->SetRegions(helper->GetReferenceSpace(level)->GetBufferedRegion());
  m_GradMask->Allocate();

  m_Metric = ImageType::New();
  m_Metric->CopyInformation(helper->GetReferenceSpace(level));
  m_Metric->SetRegions(helper->GetReferenceSpace(level)->GetBufferedRegion());
  m_Metric->Allocate();

  m_Mask = ImageType::New();
  m_Mask->CopyInformation(helper->GetReferenceSpace(level));
  m_Mask->SetRegions(helper->GetReferenceSpace(level)->GetBufferedRegion());
  m_Mask->Allocate();
}


template <unsigned int VDim, typename TReal>
void
GreedyApproach<VDim, TReal>::AffineCostFunction
::compute(const vnl_vector<double> &x, double *f, vnl_vector<double> *g)
{
  // Form a matrix/vector from x
  typename TransformType::Pointer tran = TransformType::New();

  // Divide x by the scaling
  vnl_vector<double> x_scaled = element_quotient(x, scaling);

  // Set the components of the transform
  unflatten_affine_transform(x_scaled.data_block(), tran.GetPointer());

  // Compute the gradient
  double val = 0.0;
  if(g)
    {
    vnl_vector<double> g_scaled(x_scaled.size());
    typename TransformType::Pointer grad = TransformType::New();

    if(m_Param->metric == GreedyParameters::SSD)
      {
      val = m_OFHelper->ComputeAffineMSDMatchAndGradient(
              m_Level, tran, m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, grad);

      flatten_affine_transform(grad.GetPointer(), g_scaled.data_block());
      *g = element_quotient(g_scaled, scaling);
      }
    else if(m_Param->metric == GreedyParameters::NCC)
      {

      val = m_OFHelper->ComputeAffineNCCMatchAndGradient(
              m_Level, tran, array_caster<VDim>::to_itkSize(m_Param->metric_radius),
              m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, grad);

      flatten_affine_transform(grad.GetPointer(), g_scaled.data_block());
      *g = element_quotient(g_scaled, scaling);

      // NCC should be maximized
      (*g) *= -10000.0;
      val *= -10000.0;
      }
    else if(m_Param->metric == GreedyParameters::MI)
      {
      val = m_OFHelper->ComputeAffineMIMatchAndGradient(
              m_Level, tran, m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, grad);

      flatten_affine_transform(grad.GetPointer(), g_scaled.data_block());
      *g = element_quotient(g_scaled, scaling);

      val *= -10000.0;
      (*g) *= -10000.0;

      }
    }
  else
    {
    if(m_Param->metric == GreedyParameters::SSD)
      {
      val = m_OFHelper->ComputeAffineMSDMatchAndGradient(
              m_Level, tran, m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, NULL);
      }
    else if(m_Param->metric == GreedyParameters::NCC)
      {
      val = m_OFHelper->ComputeAffineNCCMatchAndGradient(
              m_Level, tran, array_caster<VDim>::to_itkSize(m_Param->metric_radius)
              , m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, NULL);

      // NCC should be maximized
      val *= -10000.0;
      }
    else if(m_Param->metric == GreedyParameters::MI)
      {
      val = m_OFHelper->ComputeAffineMIMatchAndGradient(
              m_Level, tran, m_Metric, m_Mask, m_GradMetric, m_GradMask, m_Phi, NULL);

      val *= -10000.0;
      }
    }

  if(f)
    *f = val;
}


/*
template <unsigned int VDim, typename TReal>
void
GreedyApproach<VDim, TReal>::AffineCostFunction
::compute(const vnl_vector<double> &x, double *f, vnl_vector<double> *g)
{
  // Form a matrix/vector from x
  typename TransformType::Pointer tran = TransformType::New();

  // Set the components of the transform
  unflatten_affine_transform(x.data_block(), tran.GetPointer());

  // Compute the gradient
  double val = 0.0;
  if(g)
    {
    typename TransformType::Pointer grad = TransformType::New();
    val = m_OFHelper->ComputeAffineMatchAndGradient(m_Level, tran, grad);
    flatten_affine_transform(grad.GetPointer(), g->data_block());
    }
  else
    {
    val = m_OFHelper->ComputeAffineMatchAndGradient(m_Level, tran, NULL);
    }

  if(f)
    *f = val;
}
*
*
*/

#include "itkTransformFileReader.h"

template <typename TReal, unsigned int VDim>
vnl_matrix<TReal> ReadAffineMatrix(const TransformSpec &ts)
{
  // Physical (RAS) space transform matrix
  vnl_matrix<TReal> Qp(VDim+1, VDim+1);

  // Open the file and read the first line
  std::ifstream fin(ts.filename.c_str());
  std::string header_line, itk_header = "#Insight Transform File";
  std::getline(fin, header_line);

  if(header_line.substr(0, itk_header.size()) == itk_header)
    {
    fin.close();
    try
      {
      // First we try to load the transform using ITK code
      // This code is from c3d_affine_tool
      typedef itk::MatrixOffsetTransformBase<TReal, VDim, VDim> MOTBType;
      typedef itk::AffineTransform<TReal, VDim> AffTran;
      itk::TransformFactory<MOTBType>::RegisterTransform();
      itk::TransformFactory<AffTran>::RegisterTransform();

      itk::TransformFileReader::Pointer fltReader = itk::TransformFileReader::New();
      fltReader->SetFileName(ts.filename.c_str());
      fltReader->Update();

      itk::TransformBase *base = fltReader->GetTransformList()->front();
      typedef itk::MatrixOffsetTransformBase<TReal, VDim, VDim> MOTBType;
      MOTBType *motb = dynamic_cast<MOTBType *>(base);

      Qp.set_identity();
      if(motb)
        {
        for(size_t r = 0; r < VDim; r++)
          {
          for(size_t c = 0; c < VDim; c++)
            {
            Qp(r,c) = motb->GetMatrix()(r,c);
            }
          Qp(r,3) = motb->GetOffset()[r];
          }

        // RAS - LPI nonsense
        if(VDim == 3)
          {
          Qp(2,0) *= -1; Qp(2,1) *= -1;
          Qp(0,2) *= -1; Qp(1,2) *= -1;
          Qp(0,3) *= -1; Qp(1,3) *= -1;
          }
        }
      }
    catch(...)
      {
      throw GreedyException("Unable to read ITK transform file %s", ts.filename.c_str());
      }
    }
  else
    {
    // Try reading C3D matrix format
    fin.seekg(0);
    for(size_t i = 0; i < VDim+1; i++)
      for(size_t j = 0; j < VDim+1; j++)
        if(fin.good())
          {
          fin >> Qp[i][j];
          }
    fin.close();
    }

  // Compute the exponent
  if(ts.exponent == 1.0)
    {
    return Qp;
    }
  else if(ts.exponent == -1.0)
    {
    return vnl_matrix_inverse<TReal>(Qp);
    }
  else
    {
    throw GreedyException("Transform exponent values of +1 and -1 are the only ones currently supported");
    }

  return Qp;
}

template <unsigned int VDim, typename TReal>
void GreedyApproach<VDim, TReal>
::ReadImages(GreedyParameters &param, OFHelperType &ofhelper)
{
  // If the parameters include a sequence of transforms, apply it first
  VectorImagePointer moving_pre_warp;

  // Read the input images and stick them into an image array
  for(int i = 0; i < param.inputs.size(); i++)
    {
    // Read fixed
    typedef itk::ImageFileReader<CompositeImageType> ReaderType;
    typename ReaderType::Pointer readfix = ReaderType::New();
    readfix->SetFileName(param.inputs[i].fixed);
    readfix->Update();

    // Read moving
    typedef itk::ImageFileReader<CompositeImageType> ReaderType;
    typename ReaderType::Pointer readmov = ReaderType::New();
    readmov->SetFileName(param.inputs[i].moving);
    readmov->Update();

    // Read the pre-warps (only once)
    if(param.moving_pre_transforms.size() && moving_pre_warp.IsNull())
      {
      ReadTransformChain(param.moving_pre_transforms, readfix->GetOutput(), moving_pre_warp);
      }

    if(moving_pre_warp.IsNotNull())
      {
      // Create an image to store the warp
      CompositeImagePointer warped_moving;
      LDDMMType::alloc_cimg(warped_moving, readfix->GetOutput(),
                            readmov->GetOutput()->GetNumberOfComponentsPerPixel());

      // Interpolate the moving image using the transform chain
      LDDMMType::interp_cimg(readmov->GetOutput(), moving_pre_warp, warped_moving, false, true);

      // Add the image pair to the helper
      ofhelper.AddImagePair(readfix->GetOutput(), warped_moving, param.inputs[i].weight);
      }
    else
      {
      // Add to the helper object
      ofhelper.AddImagePair(readfix->GetOutput(), readmov->GetOutput(), param.inputs[i].weight);
      }
    }

  // Read the masks
  if(param.gradient_mask.size())
    {
    // Read gradient mask
    typedef itk::ImageFileReader<typename OFHelperType::FloatImageType> ReaderType;
    typename ReaderType::Pointer readmask = ReaderType::New();
    readmask->SetFileName(param.gradient_mask);
    readmask->Update();

    ofhelper.SetGradientMask(readmask->GetOutput());
    }
}

#include <vnl/algo/vnl_lbfgs.h>

template <unsigned int VDim, typename TReal>
vnl_matrix<double>
GreedyApproach<VDim, TReal>
::MapAffineToPhysicalRASSpace(
    OFHelperType &of_helper, int level,
    typename OFHelperType::LinearTransformType *tran)
{
  // Map the transform to NIFTI units
  vnl_matrix<double> T_fix, T_mov, Q, A;
  vnl_vector<double> s_fix, s_mov, p, b;

  GetVoxelSpaceToNiftiSpaceTransform(of_helper.GetReferenceSpace(level), T_fix, s_fix);
  GetVoxelSpaceToNiftiSpaceTransform(of_helper.GetMovingReferenceSpace(level), T_mov, s_mov);
  A = tran->GetMatrix().GetVnlMatrix();
  b = tran->GetOffset().GetVnlVector();

  Q = T_mov * A * vnl_matrix_inverse<double>(T_fix);
  p = T_mov * b + s_mov - Q * s_fix;

  vnl_matrix<double> Qp(VDim+1, VDim+1);
  Qp.set_identity();
  for(int i = 0; i < VDim; i++)
    {
    Qp(i, VDim) = p(i);
    for(int j = 0; j < VDim; j++)
      Qp(i,j) = Q(i,j);
    }

  return Qp;
}

template <unsigned int VDim, typename TReal>
void
GreedyApproach<VDim, TReal>
::MapPhysicalRASSpaceToAffine(
    OFHelperType &of_helper, int level,
    vnl_matrix<double> &Qp,
    typename OFHelperType::LinearTransformType *tran)
{
  // Map the transform to NIFTI units
  vnl_matrix<double> T_fix, T_mov, Q(VDim, VDim), A;
  vnl_vector<double> s_fix, s_mov, p(VDim), b;

  GetVoxelSpaceToNiftiSpaceTransform(of_helper.GetReferenceSpace(level), T_fix, s_fix);
  GetVoxelSpaceToNiftiSpaceTransform(of_helper.GetMovingReferenceSpace(level), T_mov, s_mov);

  for(int i = 0; i < VDim; i++)
    {
    p(i) = Qp(i, VDim);
    for(int j = 0; j < VDim; j++)
      Q(i,j) = Qp(i,j);
    }

  // A = vnl_matrix_inverse<double>(T_mov) * (Q * T_fix);
  // b = vnl_matrix_inverse<double>(T_mov) * (p - s_mov + Q * s_fix);
  A=vnl_svd<double>(T_mov).solve(Q * T_fix);
  b=vnl_svd<double>(T_mov).solve(p - s_mov + Q * s_fix);

  typename OFHelperType::LinearTransformType::MatrixType tran_A;
  typename OFHelperType::LinearTransformType::OffsetType tran_b;

  tran_A = A;
  tran_b.SetVnlVector(b);

  tran->SetMatrix(tran_A);
  tran->SetOffset(tran_b);
}



template <unsigned int VDim, typename TReal>
int GreedyApproach<VDim, TReal>
::RunAffine(GreedyParameters &param)
{
  // Create an optical flow helper object
  OFHelperType of_helper;

  // Set the scaling factors for multi-resolution
  of_helper.SetDefaultPyramidFactors(param.iter_per_level.size());

  // Read the image pairs to register
  ReadImages(param, of_helper);

  // Generate the optimized composite images
  of_helper.BuildCompositeImages(param.metric == GreedyParameters::NCC);

  // Matrix describing current transform in physical space
  vnl_matrix<double> Q_physical;

  // The number of resolution levels
  int nlevels = param.iter_per_level.size();

  // Iterate over the resolution levels
  for(unsigned int level = 0; level < nlevels; ++level)
    {
    // Define the affine cost function
    AffineCostFunction acf(&param, level, &of_helper);

    // Current transform
    typedef typename OFHelperType::LinearTransformType TransformType;
    typename TransformType::Pointer tLevel = TransformType::New();

    // Set up the initial transform
    if(level == 0)
      {
      // Use the provided initial affine as the starting point
      if(param.initial_affine.filename.length())
        {
        // Read the initial affine transform from a file
        vnl_matrix<double> Qp = ReadAffineMatrix<double, VDim>(param.initial_affine);

        // Convert the transform to voxel units
        MapPhysicalRASSpaceToAffine(of_helper, level, Qp, tLevel);
        }
      else
        {
        // Set the initial transform
        tLevel->SetIdentity();

        // Apply some random jitter to the initial transform
        vnl_vector<double> xInit = acf.GetCoefficients(tLevel);

        vnl_random rndy(12345);
        for(int i = 0; i < xInit.size(); i++)
          xInit[i] += rndy.drand32(-0.4, 0.4);

        // Map back into transform format
        acf.GetTransform(xInit, tLevel);
        }
      }
    else
      {
      // Update the transform from the last level
      MapPhysicalRASSpaceToAffine(of_helper, level, Q_physical, tLevel);
      }

    // Test derivatives
    // Convert to a parameter vector
    vnl_vector<double> xLevel = acf.GetCoefficients(tLevel.GetPointer());

    if(param.flag_debug_deriv)
      {
      // Test the gradient computation
      vnl_vector<double> xGrad(acf.get_number_of_unknowns(), 0.0);
      double f0;
      acf.compute(xLevel, &f0, &xGrad);

      // Propagate the jitter to the transform
      Q_physical = MapAffineToPhysicalRASSpace(of_helper, level, tLevel);
      std::cout << "Initial RAS Transform: " << std::endl << Q_physical  << std::endl;

      printf("ANL gradient: ");
      for(int i = 0; i < xGrad.size(); i++)
        printf("%11.4f ", xGrad[i]);
      printf("\n");

      vnl_vector<double> xGradN(acf.get_number_of_unknowns(), 0.0);
      for(int i = 0; i < acf.get_number_of_unknowns(); i++)
        {
        // double eps = (i % VDim == 0) ? 1.0e-2 : 1.0e-5;
        double eps = param.deriv_epsilon;
        double f1, f2, f3, f4;
        vnl_vector<double> x1 = xLevel, x2 = xLevel, x3 = xLevel, x4 = xLevel;
        x1[i] -= 2 * eps; x2[i] -= eps; x3[i] += eps; x4[i] += 2 * eps;

        // Four-point derivative computation
        acf.compute(x1, &f1, NULL);
        acf.compute(x2, &f2, NULL);
        acf.compute(x3, &f3, NULL);
        acf.compute(x4, &f4, NULL);

        xGradN[i] = (f1 - 8 * f2 + 8 * f3 - f4) / (12 * eps);
        }

      printf("NUM gradient: ");
      for(int i = 0; i < xGradN.size(); i++)
        printf("%11.4f ", xGradN[i]);
      printf("\n");

      std::cout << "f = " << f0 << std::endl;

      acf.GetTransform(xGrad, tLevel.GetPointer());
      std::cout << "A: " << std::endl << tLevel << std::endl;

      acf.GetTransform(xGradN, tLevel.GetPointer());
      std::cout << "N: " << std::endl << tLevel << std::endl;
      }

    // Run the minimization
    if(param.iter_per_level[level] > 0)
      {
      if(param.flag_powell)
        {
        // Set up the optimizer
        vnl_powell *optimizer = new vnl_powell(&acf);
        optimizer->set_f_tolerance(1e-9);
        optimizer->set_x_tolerance(1e-4);
        optimizer->set_g_tolerance(1e-6);
        optimizer->set_trace(true);
        optimizer->set_verbose(true);
        optimizer->set_max_function_evals(param.iter_per_level[level]);

        optimizer->minimize(xLevel);
        delete optimizer;

        }
      else
        {
        // Set up the optimizer
        vnl_lbfgs *optimizer = new vnl_lbfgs(acf);
        optimizer->set_f_tolerance(1e-9);
        optimizer->set_x_tolerance(1e-4);
        optimizer->set_g_tolerance(1e-6);
        optimizer->set_trace(true);
        optimizer->set_max_function_evals(param.iter_per_level[level]);

        optimizer->minimize(xLevel);
        delete optimizer;
        }

      // Get the final transform
      typename TransformType::Pointer tFinal = TransformType::New();
      acf.GetTransform(xLevel, tFinal.GetPointer());
      Q_physical = MapAffineToPhysicalRASSpace(of_helper, level, tFinal);
      }

    std::cout << "Final RAS Transform: " << std::endl << Q_physical << std::endl;
    }

  // Write the final affine transform
  std::ofstream matrixFile;
  matrixFile.open(param.output.c_str());
  matrixFile << Q_physical;
  matrixFile.close();


  return 0;
}

#include "itkStatisticsImageFilter.h"

template <unsigned int VDim, typename TReal>
int GreedyApproach<VDim, TReal>
::Run(GreedyParameters &param)
{
  switch(param.mode)
    {
    case GreedyParameters::GREEDY:
      return Self::RunDeformable(param);
    case GreedyParameters::AFFINE:
      return Self::RunAffine(param);
    case GreedyParameters::BRUTE:
      return Self::RunBrute(param);
    case GreedyParameters::RESLICE:
      return Self::RunReslice(param);
    }

  return -1;
}


/**
 * This is the main function of the GreedyApproach algorithm
 */
template <unsigned int VDim, typename TReal>
int GreedyApproach<VDim, TReal>
::RunDeformable(GreedyParameters &param)
{
  // Create an optical flow helper object
  OFHelperType of_helper;

  // Set the scaling factors for multi-resolution
  of_helper.SetDefaultPyramidFactors(param.iter_per_level.size());

  // Read the image pairs to register
  ReadImages(param, of_helper);

  // Generate the optimized composite images
  // TODO: why do we need to add this noise? Isn't this problematic? Figure this out.
  of_helper.BuildCompositeImages(param.metric == GreedyParameters::NCC);
  // of_helper.BuildCompositeImages(false);

  // An image pointer desribing the current estimate of the deformation
  VectorImagePointer uLevel = NULL;

  // The number of resolution levels
  int nlevels = param.iter_per_level.size();

  // Iterate over the resolution levels
  for(unsigned int level = 0; level < nlevels; ++level)
    {
    // Reference space
    ImageBaseType *refspace = of_helper.GetReferenceSpace(level);

    // Smoothing factors for this level, in physical units
    typename LDDMMType::Vec sigma_pre_phys =
        of_helper.GetSmoothingSigmasInPhysicalUnits(level, param.sigma_pre.sigma,
                                                    param.sigma_pre.physical_units);

    typename LDDMMType::Vec sigma_post_phys =
        of_helper.GetSmoothingSigmasInPhysicalUnits(level, param.sigma_post.sigma,
                                                    param.sigma_post.physical_units);

    // Report the smoothing factors used
    std::cout << "LEVEL " << level+1 << " of " << nlevels << std::endl;
    std::cout << "  Smoothing sigmas: " << sigma_pre_phys << ", " << sigma_post_phys << std::endl;

    // Intermediate images
    ImagePointer iTemp = ImageType::New();
    VectorImagePointer viTemp = VectorImageType::New();
    VectorImagePointer uk = VectorImageType::New();
    VectorImagePointer uk1 = VectorImageType::New();

    // Allocate the intermediate data
    LDDMMType::alloc_vimg(uk, refspace);
    LDDMMType::alloc_img(iTemp, refspace);
    LDDMMType::alloc_vimg(viTemp, refspace);
    LDDMMType::alloc_vimg(uk1, refspace);

    // Initialize the deformation field from last iteration
    if(uLevel.IsNotNull())
      {
      LDDMMType::vimg_resample_identity(uLevel, refspace, uk);
      LDDMMType::vimg_scale_in_place(uk, 2.0);
      uLevel = uk;
      }
    else if(param.initial_affine.filename.length())
      {
      // Read the initial affine transform from a file
      vnl_matrix<double> Qp = ReadAffineMatrix<double, VDim>(param.initial_affine);

      // Convert the transform to voxel units
      typename OFHelperType::LinearTransformType::Pointer tran = OFHelperType::LinearTransformType::New();
      MapPhysicalRASSpaceToAffine(of_helper, level, Qp, tran);

      // Create an initial warp
      OFHelperType::AffineToField(tran, uk);
      uLevel = uk;

      itk::Index<VDim> test; test.Fill(24);
      std::cout << "Index 24x24x24 maps to " << uk->GetPixel(test) << std::endl;
      }

    // Iterate for this level
    for(unsigned int iter = 0; iter < param.iter_per_level[level]; iter++)
      {

      // Compute the gradient of objective
      double total_energy;

      if(param.metric == GreedyParameters::SSD)
        {
        vnl_vector<double> all_metrics =
            of_helper.ComputeOpticalFlowField(level, uk, iTemp, uk1, param.epsilon)  / param.epsilon;

        printf("Lev:%2d  Itr:%5d  Met:[", level, iter);
        total_energy = 0.0;
        for(int i = 0;  i < all_metrics.size(); i++)
          {
          printf("  %8.6f", all_metrics[i]);
          total_energy += all_metrics[i];
          }
        printf("]  Tot: %8.6f\n", total_energy);
        }

      else if(param.metric == GreedyParameters::MI)
        {
        vnl_vector<double> all_metrics = of_helper.ComputeMIFlowField(level, uk, iTemp, uk1, param.epsilon);

        printf("Lev:%2d  Itr:%5d  Met:[", level, iter);
        total_energy = 0.0;
        for(int i = 0;  i < all_metrics.size(); i++)
          {
          printf("  %8.6f", all_metrics[i]);
          total_energy += all_metrics[i];
          }
        printf("]  Tot: %8.6f\n", total_energy);
        }

      else
        {
        itk::Size<VDim> radius = array_caster<VDim>::to_itkSize(param.metric_radius);

        // Test derivative
        // total_energy = of_helper.ComputeNCCMetricAndGradient(level, uk, uk1, radius, param.epsilon);

        /*
        if(iter == 0)
          {

          // Perform a derivative check!

          itk::Index<VDim> test; test.Fill(24);
          typename VectorImageType::PixelType vtest = uk->GetPixel(test), vv;

          itk::ImageRegion<VDim> region = uk1->GetBufferedRegion();
          // region.ShrinkByRadius(1);

          double eps = param.epsilon;
          for(int d = 0; d < VDim; d++)
            {
            vv.Fill(0.5); vv[d] -= eps; uk->FillBuffer(vv);
            of_helper.ComputeNCCMetricImage(level, uk, radius, iTemp, uk1, 1.0);

            double a1 = 0.0;
            typedef itk::ImageRegionConstIterator<ImageType> Iter;
            for(Iter it(iTemp, region); !it.IsAtEnd(); ++it)
              {
              a1 += it.Get();
              }


            vv.Fill(0.5); vv[d] += eps; uk->FillBuffer(vv);
            of_helper.ComputeNCCMetricImage(level, uk, radius, iTemp, uk1, 1.0);

            double a2 = 0.0;
            typedef itk::ImageRegionConstIterator<ImageType> Iter;
            for(Iter it(iTemp, region); !it.IsAtEnd(); ++it)
              {
              a2 += it.Get();
              }

            std::cout << "NUM:" << (a2 - a1) / (2*eps) << std::endl;

            }

          vv.Fill(0.5); uk->FillBuffer(vv);
          total_energy = of_helper.ComputeNCCMetricImage(level, uk, radius, iTemp, uk1, 1.0);
          for(int d = 0; d < VDim; d++)
            {

            double ader = 0.0;
            typedef itk::ImageRegionConstIterator<VectorImageType> Iter;
            for(Iter it(uk1, region); !it.IsAtEnd(); ++it)
              {
              ader += it.Get()[d];
              }

            // itk::Index<VDim> test; test.Fill(24);
            // std::cout << "ANA:" << uk1->GetPixel(test) << std::endl;

            std::cout << "ANA:" << ader << std::endl;
            }
          }
          */

        total_energy = of_helper.ComputeNCCMetricImage(level, uk, radius, iTemp, uk1, param.epsilon) / param.epsilon;
        printf("Level %5d,  Iter %5d:    Energy = %8.4f\n", level, iter, total_energy);
        }

      // If there is a mask, multiply the gradient by the mask
      if(param.gradient_mask.size())
        LDDMMType::vimg_multiply_in_place(uk1, of_helper.GetGradientMask(level));

      // Dump the gradient image if requested
      if(param.flag_dump_moving && 0 == iter % param.dump_frequency)
        {
        char fname[256];
        sprintf(fname, "dump_gradient_lev%02d_iter%04d.nii.gz", level, iter);
        LDDMMType::vimg_write(uk1, fname);
        }

      // We have now computed the gradient vector field. Next, we smooth it
      LDDMMType::vimg_smooth_withborder(uk1, viTemp, sigma_pre_phys, 1);

      // After smoothing, compute the maximum vector norm and use it as a normalizing
      // factor for the displacement field
      if(param.time_step_mode == GreedyParameters::SCALE)
        LDDMMType::vimg_normalize_to_fixed_max_length(viTemp, iTemp, param.epsilon, false);
      else if (param.time_step_mode == GreedyParameters::SCALEDOWN)
        LDDMMType::vimg_normalize_to_fixed_max_length(viTemp, iTemp, param.epsilon, true);

      // Dump the smoothed gradient image if requested
      if(param.flag_dump_moving && 0 == iter % param.dump_frequency)
        {
        char fname[256];
        sprintf(fname, "dump_optflow_lev%02d_iter%04d.nii.gz", level, iter);
        LDDMMType::vimg_write(viTemp, fname);
        }

      // Compute the updated deformation field - in uk1
      LDDMMType::interp_vimg(uk, viTemp, 1.0, uk1);
      LDDMMType::vimg_add_in_place(uk1, viTemp);

      // Dump if requested
      if(param.flag_dump_moving && 0 == iter % param.dump_frequency)
        {
        char fname[256];
        sprintf(fname, "dump_uk1_lev%02d_iter%04d.nii.gz", level, iter);
        LDDMMType::vimg_write(uk1, fname);
        }

      // Another layer of smoothing
      LDDMMType::vimg_smooth_withborder(uk1, uk, sigma_post_phys, 1);
      }

    // Store the end result
    uLevel = uk;

    // Compute the jacobian of the deformation field
    LDDMMType::field_jacobian_det(uk, iTemp);
    TReal jac_min, jac_max;
    LDDMMType::img_min_max(iTemp, jac_min, jac_max);
    printf("END OF LEVEL %5d    DetJac Range: %8.4f  to %8.4f \n", level, jac_min, jac_max);

    }

  // The transformation field is in voxel units. To work with ANTS, it must be mapped
  // into physical offset units - just scaled by the spacing?

  // Write the resulting transformation field
  of_helper.WriteCompressedWarpInPhysicalSpace(nlevels - 1, uLevel, param.output.c_str(), param.warp_precision);

  // If an inverse is requested, compute the inverse using the Chen 2008 fixed method.
  // A modification of this method is that if convergence is slow, we take the square
  // root of the forward transform.
  //
  // TODO: it would be more efficient to check the Lipschitz condition rather than
  // the brute force approach below
  //
  // TODO: the maximum checks should only be done over the region where the warp is
  // not going outside of the image. Right now, they are meaningless and we are doing
  // extra work when computing the inverse.
  if(param.inverse_warp.size())
    {
    // Compute the inverse
    VectorImagePointer uInverse = VectorImageType::New();
    LDDMMType::alloc_vimg(uInverse, uLevel);
    of_helper.ComputeDeformationFieldInverse(uLevel, uInverse, param.inverse_exponent);

    // Write the warp using compressed format
    of_helper.WriteCompressedWarpInPhysicalSpace(nlevels - 1, uInverse, param.inverse_warp.c_str(), param.warp_precision);
    }
  return 0;
}

/**
 * This function performs brute force search for similar patches. It generates a discrete displacement
 * field where every pixel in the fixed image is matched to the most similar pixel in the moving image
 * within a certain radius
 */
template <unsigned int VDim, typename TReal>
int GreedyApproach<VDim, TReal>
::RunBrute(GreedyParameters &param)
{
  // Check for valid parameters
  if(param.metric != GreedyParameters::NCC)
    {
    std::cerr << "Brute force search requires NCC metric only" << std::endl;
    return -1;
    }

  if(param.brute_search_radius.size() != VDim)
    {
    std::cerr << "Brute force search radius must be same dimension as the images" << std::endl;
    return -1;
    }

  // Create an optical flow helper object
  OFHelperType of_helper;

  // No multi-resolution
  of_helper.SetDefaultPyramidFactors(1);

  // Read the image pairs to register
  ReadImages(param, of_helper);

  // Generate the optimized composite images
  of_helper.BuildCompositeImages(true);

  // Reference space
  ImageBaseType *refspace = of_helper.GetReferenceSpace(0);

  // Intermediate images
  VectorImagePointer u_best = VectorImageType::New();
  VectorImagePointer u_curr = VectorImageType::New();
  ImagePointer m_curr = ImageType::New();
  ImagePointer m_best = ImageType::New();

  // Allocate the intermediate data
  LDDMMType::alloc_vimg(u_best, refspace);
  LDDMMType::alloc_vimg(u_curr, refspace);
  LDDMMType::alloc_img(m_best, refspace);
  LDDMMType::alloc_img(m_curr, refspace);

  // Allocate m_best to a negative value
  m_best->FillBuffer(-100.0);

  // Create a neighborhood for computing offsets
  itk::Neighborhood<float, VDim> dummy_nbr;
  itk::Size<VDim> search_rad = array_caster<VDim>::to_itkSize(param.brute_search_radius);
  itk::Size<VDim> metric_rad = array_caster<VDim>::to_itkSize(param.metric_radius);
  dummy_nbr.SetRadius(search_rad);

  // Iterate over all offsets
  for(int k = 0; k < dummy_nbr.Size(); k++)
    {
    // Get the offset corresponding to this iteration
    itk::Offset<VDim> offset = dummy_nbr.GetOffset(k);

    // Fill the deformation field with this offset
    typename LDDMMType::Vec vec_offset;
    for(int i = 0; i < VDim; i++)
      vec_offset[i] = offset[i];
    u_curr->FillBuffer(vec_offset);

    // Perform interpolation and metric computation
    of_helper.ComputeNCCMetricImage(0, u_curr, metric_rad, m_curr);

    // Temp: keep track of number of updates
    unsigned long n_updates = 0;

    // Out of laziness, just take a quick pass over the images
    typename VectorImageType::RegionType rgn = refspace->GetBufferedRegion();
    itk::ImageRegionIterator<VectorImageType> it_u(u_best, rgn);
    itk::ImageRegionConstIterator<ImageType> it_m_curr(m_curr, rgn);
    itk::ImageRegionIterator<ImageType> it_m_best(m_best, rgn);
    for(; !it_m_best.IsAtEnd(); ++it_m_best, ++it_m_curr, ++it_u)
      {
      float v_curr = it_m_curr.Value();
      if(v_curr > it_m_best.Value())
        {
        it_m_best.Set(v_curr);
        it_u.Set(vec_offset);
        ++n_updates;
        }
      }

    std::cout << "offset: " << offset << "     updates: " << n_updates << std::endl;
    }

  LDDMMType::vimg_write(u_best, param.output.c_str());
  LDDMMType::img_write(m_best, "mbest.nii.gz");

  return 0;
}


#include "itkWarpVectorImageFilter.h"
#include "itkWarpImageFilter.h"
#include "itkNearestNeighborInterpolateImageFunction.h"


template <unsigned int VDim, typename TReal>
void GreedyApproach<VDim, TReal>
::ReadTransformChain(const std::vector<TransformSpec> &tran_chain,
                     ImageBaseType *ref_space,
                     VectorImagePointer &out_warp)
{
  // Create the initial transform and set it to zero
  out_warp = VectorImageType::New();
  LDDMMType::alloc_vimg(out_warp, ref_space);

  // Read the sequence of transforms
  for(int i = 0; i < tran_chain.size(); i++)
    {
    // Read the next parameter
    std::string tran = tran_chain[i].filename;

    // Determine if it's an affine transform
    if(itk::ImageIOFactory::CreateImageIO(tran.c_str(), itk::ImageIOFactory::ReadMode))
      {
      // Create a temporary warp
      VectorImagePointer warp_tmp = VectorImageType::New();
      LDDMMType::alloc_vimg(warp_tmp, ref_space);

      // Read the next warp
      VectorImagePointer warp_i = VectorImageType::New();
      LDDMMType::vimg_read(tran.c_str(), warp_i);

      // Now we need to compose the current transform and the overall warp.
      LDDMMType::interp_vimg(warp_i, out_warp, 1.0, warp_tmp, false, true);
      LDDMMType::vimg_add_in_place(out_warp, warp_tmp);
      }
    else
      {
      // Read the transform as a matrix
      vnl_matrix<TReal> mat = ReadAffineMatrix<TReal, VDim>(tran_chain[i]);
      vnl_matrix<double>  A = mat.extract(VDim, VDim);
      vnl_vector<double> b = mat.get_column(VDim).extract(VDim), q;

      // TODO: stick this in a filter to take advantage of threading!
      typedef itk::ImageRegionIteratorWithIndex<VectorImageType> IterType;
      for(IterType it(out_warp, out_warp->GetBufferedRegion()); !it.IsAtEnd(); ++it)
        {
        typename VectorImageType::PointType pt, pt2;
        typename VectorImageType::IndexType idx = it.GetIndex();

        // Get the physical position
        // TODO: this calls IsInside() internally, which limits efficiency
        out_warp->TransformIndexToPhysicalPoint(idx, pt);

        // Add the displacement (in DICOM coordinates) and
        for(int i = 0; i < VDim; i++)
          pt2[i] = pt[i] + it.Value()[i];

        // Switch to NIFTI coordinates
        pt2[0] = -pt2[0]; pt2[1] = -pt2[1];

        // Apply the matrix - get the transformed coordinate in DICOM space
        q = A * pt2.GetVnlVector() + b;
        q[0] = -q[0]; q[1] = -q[1];

        // Compute the difference in DICOM space
        for(int i = 0; i < VDim; i++)
          it.Value()[i] = q[i] - pt[i];
        }
      }
    }
}

#include "itkBinaryThresholdImageFilter.h"
#include "itkRecursiveGaussianImageFilter.h"
#include "itkNaryFunctorImageFilter.h"

template <class TInputImage, class TOutputImage>
class NaryLabelVotingFunctor
{
public:
  typedef NaryLabelVotingFunctor<TInputImage,TOutputImage> Self;
  typedef typename TInputImage::PixelType InputPixelType;
  typedef typename TOutputImage::PixelType OutputPixelType;
  typedef std::vector<OutputPixelType> LabelArray;

  NaryLabelVotingFunctor(const LabelArray &labels)
    : m_LabelArray(labels), m_Size(labels.size()) {}

  NaryLabelVotingFunctor() : m_Size(0) {}


  OutputPixelType operator() (const std::vector<InputPixelType> &pix)
  {
    InputPixelType best_val = pix[0];
    int best_index = 0;
    for(int i = 1; i < m_Size; i++)
      if(pix[i] > best_val)
        {
        best_val = pix[i];
        best_index = i;
        }

    return m_LabelArray[best_index];
  }

  bool operator != (const Self &other)
    { return other.m_LabelArray != m_LabelArray; }

protected:
  LabelArray m_LabelArray;
  int m_Size;
};

/**
 * Run the reslice code - simply apply a warp or set of warps to images
 */
template <unsigned int VDim, typename TReal>
int GreedyApproach<VDim, TReal>
::RunReslice(GreedyParameters &param)
{
  typedef typename OFHelperType::LinearTransformType TransformType;

  GreedyResliceParameters r_param = param.reslice_param;

  // Check the parameters
  if(!r_param.ref_image.size())
    throw GreedyException("A reference image (-rf) option is required for reslice commands");

  if(!r_param.images.size())
    throw GreedyException("At least one pair of moving/output images (-rm) is required for reslice commands");

  // Read the fixed as a plain image (we don't care if it's composite)
  ImagePointer ref = ImageType::New();
  LDDMMType::img_read(r_param.ref_image.c_str(), ref);
  itk::ImageBase<VDim> *ref_space = ref;

  // Read the transform chain
  VectorImagePointer warp;
  ReadTransformChain(param.reslice_param.transforms, ref_space, warp);

  // Process image pairs
  for(int i = 0; i < r_param.images.size(); i++)
    {
    const char *filename = r_param.images[i].moving.c_str();

    // Handle the special case of multi-label images
    if(r_param.images[i].interp.mode == InterpSpec::LABELWISE)
      {
      // The label image assumed to be an image of shorts
      typedef itk::Image<short, VDim> LabelImageType;
      typedef itk::ImageFileReader<LabelImageType> LabelReaderType;

      // Create a reader
      typename LabelReaderType::Pointer reader = LabelReaderType::New();
      reader->SetFileName(filename);
      reader->Update();
      typename LabelImageType::Pointer moving = reader->GetOutput();

      // Scan the unique labels in the image
      std::set<short> label_set;
      short *labels = moving->GetBufferPointer();
      int n_pixels = moving->GetPixelContainer()->Size();

      // Get the list of unique pixels
      short last_pixel = 0;
      for(int j = 0; j < n_pixels; j++)
        {
        short pixel = labels[j];
        if(last_pixel != pixel || i == 0)
          {
          label_set.insert(pixel);
          last_pixel = pixel;
          if(label_set.size() > 1000)
            throw GreedyException("Label wise interpolation not supported for image %s "
                                  "which has over 1000 distinct labels", filename);
          }
        }

      // Turn this set into an array
      std::vector<short> label_array(label_set.begin(), label_set.end());

      // Create a N-way voting filter
      typedef NaryLabelVotingFunctor<ImageType, LabelImageType> VotingFunctor;
      VotingFunctor vf(label_array);

      typedef itk::NaryFunctorImageFilter<ImageType, LabelImageType, VotingFunctor> VotingFilter;
      typename VotingFilter::Pointer fltVoting = VotingFilter::New();
      fltVoting->SetFunctor(vf);

      // Create a mini-pipeline of streaming filters
      for(int j = 0; j < label_array.size(); j++)
        {
        // Set up a threshold filter for this label
        typedef itk::BinaryThresholdImageFilter<LabelImageType, ImageType> ThresholdFilterType;
        typename ThresholdFilterType::Pointer fltThreshold = ThresholdFilterType::New();
        fltThreshold->SetInput(moving);
        fltThreshold->SetLowerThreshold(label_array[j]);
        fltThreshold->SetUpperThreshold(label_array[j]);
        fltThreshold->SetInsideValue(1.0);
        fltThreshold->SetOutsideValue(0.0);

        // Set up a smoothing filter for this label
        // TODO: sigma is currently in world units - bad!
        typedef itk::RecursiveGaussianImageFilter<ImageType, ImageType> SmootherType;
        typename SmootherType::Pointer fltSmooth = SmootherType::New();
        fltSmooth->SetInput(fltThreshold->GetOutput());
        fltSmooth->SetSigma(r_param.images[i].interp.sigma);

        // TODO: we should really be coercing the output into a vector image to speed up interpolation!
        typedef FastWarpCompositeImageFilter<ImageType, ImageType, VectorImageType> InterpFilter;
        typename InterpFilter::Pointer fltInterp = InterpFilter::New();
        fltInterp->SetMovingImage(fltSmooth->GetOutput());
        fltInterp->SetDeformationField(warp);
        fltInterp->SetUsePhysicalSpace(true);

        fltInterp->Update();

        // Add to the voting filter
        fltVoting->SetInput(j, fltInterp->GetOutput());
        }

      // TODO: test out streaming!
      // Run this big pipeline
      fltVoting->Update();

      // Save
      typedef itk::ImageFileWriter<LabelImageType> WriterType;
      typename WriterType::Pointer writer = WriterType::New();
      writer->SetFileName(r_param.images[i].output.c_str());
      writer->SetInput(fltVoting->GetOutput());
      writer->Update();
      }
    else
      {
      // Read the input image
      CompositeImagePointer moving, warped;
      itk::ImageIOBase::IOComponentType comp =
          LDDMMType::cimg_read(filename, moving);

      // Allocate the warped image
      LDDMMType::alloc_cimg(warped, ref_space, moving->GetNumberOfComponentsPerPixel());

      // Perform the warp
      LDDMMType::interp_cimg(moving, warp, warped,
                             r_param.images[i].interp.mode == InterpSpec::NEAREST,
                             true);

      // Write, casting to the input component type
      LDDMMType::cimg_write(warped, r_param.images[i].output.c_str(), comp);
      }
    }


  return 0;
}




#include "itksys/SystemTools.hxx"

class CommandLineHelper
{
public:
  CommandLineHelper(int argc, char *argv[])
  {
    this->argc = argc;
    this->argv = argv;
    i = 1;
  }

  bool is_at_end()
  {
    return i >= argc;
  }

  /**
   * Just read the next arg (used internally)
   */
  const char *read_arg()
  {
    if(i >= argc)
      throw GreedyException("Unexpected end of command line arguments.");

    return argv[i++];
  }

  /**
   * Read a command (something that starts with a '-')
   */
  std::string read_command()
  {
    current_command = read_arg();
    if(current_command[0] != '-')
      throw GreedyException("Expected a command at position %d, instead got '%s'.", i, current_command.c_str());
    return current_command;
  }

  /**
   * Read a string that is not a command (may not start with a -)
   */
  std::string read_string()
  {
    std::string arg = read_arg();
    if(arg[0] == '-')
      throw GreedyException("Expected a string argument as parameter to '%s', instead got '%s'.", current_command.c_str(), arg.c_str());

    return arg;
  }


  /**
   * Get the number of free arguments to the current command. Use only for commands with
   * a priori unknown number of arguments. Otherwise, just use the get_ commands
   */
  int command_arg_count(int min_required = 0)
  {
    // Count the number of arguments
    int n_args = 0;
    for(int j = i; j < argc; j++, n_args++)
      if(argv[j][0] == '-')
        break;

    // Test for minimum required
    if(n_args < min_required)
      throw GreedyException(
          "Expected at least %d arguments to '%s', instead got '%d'",
          min_required, current_command.c_str(), n_args);

    return n_args;
  }

  /**
   * Read an existing filename
   */
  std::string read_existing_filename()
  {
    std::string file = read_arg();
    if(!itksys::SystemTools::FileExists(file.c_str()))
      throw GreedyException("File '%s' does not exist", file.c_str());

    return file;
  }

  /**
   * Read a transform specification, format file,number
   */
  TransformSpec read_transform_spec()
  {
    std::string spec = read_arg();
    size_t pos = spec.find_first_of(',');

    TransformSpec ts;
    ts.filename = spec.substr(0, pos);
    ts.exponent = 1.0;

    if(!itksys::SystemTools::FileExists(ts.filename.c_str()))
      throw GreedyException("File '%s' does not exist", ts.filename.c_str());

    if(pos != std::string::npos)
      {
      errno = 0; char *pend;
      std::string expstr = spec.substr(pos+1);
      ts.exponent = std::strtod(expstr.c_str(), &pend);

      if(errno || *pend)
        throw GreedyException("Expected a floating point number after comma in transform specification '%s', instead got '%s'",
                              current_command.c_str(), spec.substr(pos).c_str());

      }

    return ts;
  }

  /**
   * Read an output filename
   */
  std::string read_output_filename()
  {
    std::string file = read_arg();
    return file;
  }

  /**
   * Read a floating point value
   */
  double read_double()
  {
    std::string arg = read_arg();

    errno = 0; char *pend;
    double val = std::strtod(arg.c_str(), &pend);

    if(errno || *pend)
      throw GreedyException("Expected a floating point number as parameter to '%s', instead got '%s'",
                            current_command.c_str(), arg.c_str());

    return val;
  }

  /**
   * Check if a string ends with another string and return the
   * substring without the suffix
   */
  bool check_suffix(const std::string &source, const std::string &suffix, std::string &out_prefix)
  {
    int n = source.length(), m = suffix.length();
    if(n < m)
      return false;

    if(source.substr(n-m, m) != suffix)
      return false;

    out_prefix = source.substr(0, n-m);
    return true;
  }

  /**
   * Read a floating point value with units (mm or vox)
   */
  double read_scalar_with_units(bool &physical_units)
  {
    std::string arg = read_arg();
    std::string scalar;

    if(check_suffix(arg, "vox", scalar))
      physical_units = false;
    else if(check_suffix(arg, "mm", scalar))
      physical_units = true;
    else
      throw GreedyException("Parameter to '%s' should include units, e.g. '3vox' or '3mm', instead got '%s'",
                            current_command.c_str(), arg.c_str());

    errno = 0; char *pend;
    double val = std::strtod(scalar.c_str(), &pend);

    if(errno || *pend)
      throw GreedyException("Expected a floating point number as parameter to '%s', instead got '%s'",
                            current_command.c_str(), scalar.c_str());

    return val;
  }

  /**
   * Read an integer value
   */
  long read_integer()
  {
    std::string arg = read_arg();

    errno = 0; char *pend;
    long val = std::strtol(arg.c_str(), &pend, 10);

    if(errno || *pend)
      throw GreedyException("Expected an integer as parameter to '%s', instead got '%s'",
                            current_command.c_str(), arg.c_str());

    return val;
  }

  /**
   * Read one of a list of strings. The optional parameters to this are in the form
   * int, string, int, string, int, string. Each string may in turn contain a list
   * of words (separated by space) that are acceptable. So for example. NULL string
   * is used to refer to the default option.
   *
   * enum Mode { NORMAL, BAD, SILLY }
   * Mode m = X.read_option(NORMAL, "NORMAL normal", BAD, "bad BAD", SILLY, NULL);
   */
  /*
  template <class TOption>
  TOption read_option(TOption opt1, const char *str1, ...)
  {
    not implemented yet
  }
  */

  /**
   * Read a vector in the format 1.0x0.2x0.6
   */
  std::vector<double> read_double_vector()
  {
    std::string arg = read_arg();
    std::istringstream f(arg);
    std::string s;
    std::vector<double> vector;
    while (getline(f, s, 'x'))
      {
      errno = 0; char *pend;
      double val = std::strtod(s.c_str(), &pend);

      if(errno || *pend)
        throw GreedyException("Expected a floating point vector as parameter to '%s', instead got '%s'",
                              current_command.c_str(), arg.c_str());
      vector.push_back(val);
      }

    if(!vector.size())
      throw GreedyException("Expected a floating point vector as parameter to '%s', instead got '%s'",
                            current_command.c_str(), arg.c_str());

    return vector;
  }

  std::vector<int> read_int_vector()
  {
    std::string arg = read_arg();
    std::istringstream f(arg);
    std::string s;
    std::vector<int> vector;
    while (getline(f, s, 'x'))
      {
      errno = 0; char *pend;
      long val = std::strtol(s.c_str(), &pend, 10);

      if(errno || *pend)
        throw GreedyException("Expected an integer vector as parameter to '%s', instead got '%s'",
                              current_command.c_str(), arg.c_str());
      vector.push_back((int) val);
      }

    if(!vector.size())
      throw GreedyException("Expected an integer vector as parameter to '%s', instead got '%s'",
                            current_command.c_str(), arg.c_str());

    return vector;
  }





private:
  int argc, i;
  char **argv;
  std::string current_command;
};

int main(int argc, char *argv[])
{
  GreedyParameters param;
  double current_weight = 1.0;

  param.dim = 2;
  param.mode = GreedyParameters::GREEDY;
  param.flag_dump_moving = false;
  param.flag_debug_deriv = false;
  param.dump_frequency = 1;
  param.epsilon = 1.0;
  param.sigma_pre.sigma = sqrt(3.0);
  param.sigma_pre.physical_units = false;
  param.sigma_post.sigma = sqrt(0.5);
  param.sigma_post.physical_units = false;
  param.threads = 0;
  param.metric = GreedyParameters::SSD;
  param.time_step_mode = GreedyParameters::SCALE;
  param.deriv_epsilon = 1e-4;
  param.flag_powell = false;
  param.inverse_exponent = 2;
  param.warp_precision = 0.1;

  // reslice mode parameters
  InterpSpec interp_current;

  param.iter_per_level.push_back(100);
  param.iter_per_level.push_back(100);

  if(argc < 3)
    return usage();

  try
  {
    CommandLineHelper cl(argc, argv);
    while(!cl.is_at_end())
      {
      // Read the next command
      std::string arg = cl.read_command();

      if(arg == "-d")
        {
        param.dim = cl.read_integer();
        }
      else if(arg == "-n")
        {
        param.iter_per_level = cl.read_int_vector();
        }
      else if(arg == "-w")
        {
        current_weight = cl.read_double();
        }
      else if(arg == "-e")
        {
        param.epsilon = cl.read_double();
        }
      else if(arg == "-m")
        {
        std::string metric_name = cl.read_string();
        if(metric_name == "NCC" || metric_name == "ncc")
          {
          param.metric = GreedyParameters::NCC;
          param.metric_radius = cl.read_int_vector();
          }
        else if(metric_name == "MI" || metric_name == "mi")
          {
          param.metric = GreedyParameters::MI;
          }
        }
      else if(arg == "-tscale")
        {
        std::string mode = cl.read_string();
        if(mode == "SCALE" || mode == "scale")
          param.time_step_mode = GreedyParameters::SCALE;
        else if(mode == "SCALEDOWN" || mode == "scaledown")
          param.time_step_mode = GreedyParameters::SCALEDOWN;
        }
      else if(arg == "-s")
        {
        param.sigma_pre.sigma = cl.read_scalar_with_units(param.sigma_pre.physical_units);
        param.sigma_post.sigma = cl.read_scalar_with_units(param.sigma_post.physical_units);
        }
      else if(arg == "-i")
        {
        ImagePairSpec ip;
        ip.weight = current_weight;
        ip.fixed = cl.read_existing_filename();
        ip.moving = cl.read_existing_filename();
        param.inputs.push_back(ip);
        }
      else if(arg == "-ia")
        {
        param.initial_affine = cl.read_transform_spec();
        }
      else if(arg == "-it")
        {
        int nFiles = cl.command_arg_count();
        for(int i = 0; i < nFiles; i++)
          param.moving_pre_transforms.push_back(cl.read_transform_spec());
        }
      else if(arg == "-gm")
        {
        param.gradient_mask = cl.read_existing_filename();
        }
      else if(arg == "-o")
        {
        param.output = cl.read_output_filename();
        }
      else if(arg == "-dump-moving")
        {
        param.flag_dump_moving = true;
        }
      else if(arg == "-powell")
        {
        param.flag_powell = true;
        }
      else if(arg == "-dump-frequency" || arg == "-dump-freq")
        {
        param.dump_frequency = cl.read_integer();
        }
      else if(arg == "-debug-deriv")
        {
        param.flag_debug_deriv = true;
        }
      else if(arg == "-debug-deriv-eps")
        {
        param.deriv_epsilon = cl.read_double();
        }
      else if(arg == "-threads")
        {
        param.threads = cl.read_integer();
        }
      else if(arg == "-a")
        {
        param.mode = GreedyParameters::AFFINE;
        }
      else if(arg == "-brute")
        {
        param.mode = GreedyParameters::BRUTE;
        param.brute_search_radius = cl.read_int_vector();
        }
      else if(arg == "-r")
        {
        param.mode = GreedyParameters::RESLICE;
        int nFiles = cl.command_arg_count();
        for(int i = 0; i < nFiles; i++)
          param.reslice_param.transforms.push_back(cl.read_transform_spec());
        }
      else if(arg == "-rm")
        {
        ResliceSpec rp;
        rp.interp = interp_current;
        rp.moving = cl.read_existing_filename();
        rp.output = cl.read_output_filename();
        param.reslice_param.images.push_back(rp);
        }
      else if(arg == "-rf")
        {
        param.reslice_param.ref_image = cl.read_existing_filename();
        }
      else if(arg == "-oinv")
        {
        param.inverse_warp = cl.read_output_filename();
        }
      else if(arg == "-invexp")
        {
        param.inverse_exponent = cl.read_integer();
        }
      else if(arg == "-ri")
        {
        std::string mode = cl.read_string();
        if(mode == "nn" || mode == "NN" || mode == "0")
          {
          interp_current.mode = InterpSpec::NEAREST;
          }
        else if(mode == "linear" || mode == "LINEAR" || mode == "1")
          {
          interp_current.mode = InterpSpec::LINEAR;
          }
        else if(mode == "label" || mode == "LABEL")
          {
          interp_current.mode = InterpSpec::LABELWISE;
          interp_current.sigma = cl.read_double();
          }
        else
          {
          std::cerr << "Unknown interpolation mode" << std::endl;
          }
        }
      else if(arg == "-wp")
        {
        param.warp_precision = cl.read_double();
        }
      else
        {
        std::cerr << "Unknown parameter " << arg << std::endl;
        return -1;
        }
      }

    // Use the threads parameter
    if(param.threads > 0)
      {
      std::cout << "Limiting the number of threads to " << param.threads << std::endl;
      itk::MultiThreader::SetGlobalMaximumNumberOfThreads(param.threads);
      }
    else
      {
      std::cout << "Executing with the default number of threads: " << itk::MultiThreader::GetGlobalDefaultNumberOfThreads() << std::endl;

      }

    // Run the main code
    switch(param.dim)
      {
      case 2: return GreedyApproach<2, double>::Run(param); break;
      case 3: return GreedyApproach<3, double>::Run(param); break;
      case 4: return GreedyApproach<4, double>::Run(param); break;
      default: throw GreedyException("Wrong number of dimensions requested: %d", param.dim);
      }
  }
  catch(std::exception &exc)
  {
    std::cerr << "ABORTING PROGRAM DUE TO RUNTIME EXCEPTION -- "
              << exc.what() << std::endl;
    return -1;
  }
}
