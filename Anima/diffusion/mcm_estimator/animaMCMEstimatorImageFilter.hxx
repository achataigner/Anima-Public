#pragma once
#include "animaMCMEstimatorImageFilter.h"
#include <animaMCMConstants.h>

#include <itkImageRegionIterator.h>
#include <itkImageRegionConstIterator.h>

#include <animaNLOPTOptimizers.h>
#include <itkLevenbergMarquardtOptimizer.h>
#include <animaNNOrthogonalMatchingPursuitOptimizer.h>

#include <animaMCMSingleValuedCostFunction.h>
#include <animaMCMMultipleValuedCostFunction.h>
#include <animaGaussianMCMCost.h>
#include <animaGaussianMCMVariableProjectionCost.h>

#include <animaVectorOperations.h>
#include <animaSphereOperations.h>

#include <animaBaseTensorTools.h>
#include <animaMCMFileWriter.h>

#include <limits>

namespace anima
{

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::AddGradientDirection(unsigned int i, GradientType &grad)
{
    if (i == m_GradientDirections.size())
        m_GradientDirections.push_back(grad);
    else if (i > m_GradientDirections.size())
        std::cerr << "Trying to add a direction not contiguous... Add directions contiguously (0,1,2,3,...)..." << std::endl;
    else
        m_GradientDirections[i] = grad;
}

template <class InputPixelType, class OutputPixelType>
typename MCMEstimatorImageFilter<InputPixelType, OutputPixelType>::MCMCreatorType *
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::GetNewMCMCreatorInstance()
{
    return new MCMCreatorType;
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::GenerateOutputInformation()
{
    // Override the method in itkImageSource, so we can set the vector length of
    // the output itk::VectorImage

    this->Superclass::GenerateOutputInformation();

    OutputImageType *output = this->GetOutput();

    // Create fake MCM output to get its length
    MCMCreatorType *tmpMCMCreator = this->GetNewMCMCreatorInstance();
    tmpMCMCreator->SetModelWithFreeWaterComponent(m_ModelWithFreeWaterComponent);
    tmpMCMCreator->SetModelWithStationaryWaterComponent(m_ModelWithStationaryWaterComponent);
    tmpMCMCreator->SetModelWithRestrictedWaterComponent(m_ModelWithRestrictedWaterComponent);
    tmpMCMCreator->SetModelWithStaniszComponent(m_ModelWithStaniszComponent);
    tmpMCMCreator->SetCompartmentType(m_CompartmentType);
    tmpMCMCreator->SetNumberOfCompartments(m_NumberOfCompartments);

    MCMPointer tmpMCM = tmpMCMCreator->GetNewMultiCompartmentModel();

    output->SetVectorLength(tmpMCM->GetSize());
    output->SetDescriptionModel(tmpMCM);
    
    delete tmpMCMCreator;
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::WriteMCMOutput(std::string fileName)
{
    MCMPointer tmpMCM = m_MCMCreators[0]->GetNewMultiCompartmentModel();

    typedef anima::MCMFileWriter <OutputPixelType, InputImageType::ImageDimension> MCMFileWriterType;
    MCMFileWriterType writer;

    writer.SetInputImage(this->GetOutput());
    writer.SetFileName(fileName);

    writer.Update();
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::CheckComputationMask()
{
    typedef itk::ImageRegionConstIterator <InputImageType> B0IteratorType;
    typedef itk::ImageRegionIterator <MaskImageType> MaskIteratorType;

    unsigned int firstB0Index = 0;
    double bValueFirstB0Index = anima::GetBValueFromAcquisitionParameters(m_SmallDelta, m_BigDelta, m_GradientStrengths[firstB0Index]);
    while (bValueFirstB0Index > 10)
    {
        ++firstB0Index;
        bValueFirstB0Index = anima::GetBValueFromAcquisitionParameters(m_SmallDelta, m_BigDelta, m_GradientStrengths[firstB0Index]);
    }

    B0IteratorType b0Itr(this->GetInput(firstB0Index),this->GetOutput()->GetLargestPossibleRegion());

    if (!this->GetComputationMask())
        this->Superclass::CheckComputationMask();

    MaskIteratorType maskItr(this->GetComputationMask(),this->GetOutput()->GetLargestPossibleRegion());

    while (!b0Itr.IsAtEnd())
    {
        if ((maskItr.Get() != 0)&&(b0Itr.Get() <= m_B0Threshold))
            maskItr.Set(0);

        ++b0Itr;
        ++maskItr;
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::BeforeThreadedGenerateData()
{
    if ((m_Optimizer == "levenberg")&&((m_MLEstimationStrategy == Marginal)||(m_NoiseType != Gaussian)))
        itkExceptionMacro("Levenberg Marquardt optimizer only working with Gaussian noise and variable projection");

    if ((m_Optimizer != "bobyqa")&&(m_UseCommonDiffusivities||m_UseCommonExtraAxonalFractions||m_UseCommonConcentrations))
        itkExceptionMacro("Derivative based optimizers not supported yet for common parameters, use Bobyqa instead");

    if ((m_NoiseType == NCC)&&(m_MLEstimationStrategy != Profile))
        itkExceptionMacro("NCC noise is only compatible with profile estimation strategy");

    srand(time(0));

    m_NumberOfImages = this->GetNumberOfIndexedInputs();

    if (m_GradientStrengths.size() != m_NumberOfImages)
        itkExceptionMacro("There should be the same number of input images and input b-values...");

    itk::ImageRegionIterator <OutputImageType> fillOut(this->GetOutput(),this->GetOutput()->GetLargestPossibleRegion());
    unsigned int outSize = this->GetOutput()->GetNumberOfComponentsPerPixel();
    typename OutputImageType::PixelType emptyModelVec(outSize);
    emptyModelVec.Fill(0);

    while (!fillOut.IsAtEnd())
    {
        fillOut.Set(emptyModelVec);
        ++fillOut;
    }

    // Create AICc volume
    m_AICcVolume = OutputScalarImageType::New();
    m_AICcVolume->Initialize();
    m_AICcVolume->SetRegions(this->GetInput(0)->GetLargestPossibleRegion());
    m_AICcVolume->SetSpacing (this->GetInput(0)->GetSpacing());
    m_AICcVolume->SetOrigin (this->GetInput(0)->GetOrigin());
    m_AICcVolume->SetDirection (this->GetInput(0)->GetDirection());
    m_AICcVolume->Allocate();
    m_AICcVolume->FillBuffer(0);

    // Create B0 volume
    m_B0Volume = OutputScalarImageType::New();
    m_B0Volume->Initialize();
    m_B0Volume->SetRegions(this->GetInput(0)->GetLargestPossibleRegion());
    m_B0Volume->SetSpacing (this->GetInput(0)->GetSpacing());
    m_B0Volume->SetOrigin (this->GetInput(0)->GetOrigin());
    m_B0Volume->SetDirection (this->GetInput(0)->GetDirection());
    m_B0Volume->Allocate();
    m_B0Volume->FillBuffer(0);

    // Create sigma volume
    m_SigmaSquareVolume = OutputScalarImageType::New();
    m_SigmaSquareVolume->Initialize();
    m_SigmaSquareVolume->SetRegions(this->GetInput(0)->GetLargestPossibleRegion());
    m_SigmaSquareVolume->SetSpacing (this->GetInput(0)->GetSpacing());
    m_SigmaSquareVolume->SetOrigin (this->GetInput(0)->GetOrigin());
    m_SigmaSquareVolume->SetDirection (this->GetInput(0)->GetDirection());
    m_SigmaSquareVolume->Allocate();
    m_SigmaSquareVolume->FillBuffer(0);

    // Create mose volume
    if (!m_MoseVolume)
    {
        m_MoseVolume = MoseImageType::New();
        m_MoseVolume->Initialize();
        m_MoseVolume->SetRegions(this->GetInput(0)->GetLargestPossibleRegion());
        m_MoseVolume->SetSpacing (this->GetInput(0)->GetSpacing());
        m_MoseVolume->SetOrigin (this->GetInput(0)->GetOrigin());
        m_MoseVolume->SetDirection (this->GetInput(0)->GetDirection());
        m_MoseVolume->Allocate();
        m_MoseVolume->FillBuffer(0);
    }

    if (m_ExternalMoseVolume)
        m_FindOptimalNumberOfCompartments = false;

    Superclass::BeforeThreadedGenerateData();

    m_MCMCreators.resize(this->GetNumberOfThreads());
    for (unsigned int i = 0;i < this->GetNumberOfThreads();++i)
        m_MCMCreators[i] = this->GetNewMCMCreatorInstance();
    
    std::cout << "Initial diffusivities:" << std::endl;
    std::cout << " - Axial diffusivity: " << m_AxialDiffusivityValue << " mm2/s," << std::endl;
    std::cout << " - Radial diffusivity 1: " << m_RadialDiffusivity1Value << " mm2/s," << std::endl;
    std::cout << " - Radial diffusivity 2: " << m_RadialDiffusivity2Value << " mm2/s," << std::endl;

    if (m_ModelWithRestrictedWaterComponent)
        std::cout << " - IRW diffusivity: " << m_IRWDiffusivityValue << " mm2/s," << std::endl;

    if (m_ModelWithStaniszComponent)
        std::cout << " - Stanisz diffusivity: " << m_StaniszDiffusivityValue << " mm2/s," << std::endl;

    // Setting up creators
    if (m_Optimizer == "levenberg")
        m_UseBoundedOptimization = true;

    for (unsigned int i = 0;i < this->GetNumberOfThreads();++i)
    {
        m_MCMCreators[i]->SetAxialDiffusivityValue(m_AxialDiffusivityValue);
        m_MCMCreators[i]->SetFreeWaterDiffusivityValue(3.0e-3);
        m_MCMCreators[i]->SetIRWDiffusivityValue(m_IRWDiffusivityValue);
        m_MCMCreators[i]->SetStaniszDiffusivityValue(m_StaniszDiffusivityValue);
        m_MCMCreators[i]->SetRadialDiffusivity1Value(m_RadialDiffusivity1Value);
        m_MCMCreators[i]->SetRadialDiffusivity2Value(m_RadialDiffusivity2Value);
        m_MCMCreators[i]->SetUseBoundedOptimization(m_UseBoundedOptimization);
    }
    
    // Switch over compartment types to setup coarse grid initialization
    switch (m_CompartmentType)
    {
        case NODDI:
        case DDI:
            this->ComputeExtraAxonalAndKappaCoarseGrids();
            break;
            
        case Tensor:
            this->ComputeTensorRadialDiffsAndAzimuthCoarseGrids();
            break;
            
        default:
            // No coarse grid initialization for simple models
            break;
    }

    // Sparse pre-computation
    this->InitializeDictionary();
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::InitializeDictionary()
{
    anima::GetSphereEvenSampling(m_DictionaryDirections,m_NumberOfDictionaryEntries);
    std::vector <double> fakeIsotropicDirection(3,0);

    unsigned int countIsoComps = 0;
    if (m_ModelWithFreeWaterComponent)
    {
        m_DictionaryDirections.insert(m_DictionaryDirections.begin(),fakeIsotropicDirection);
        ++countIsoComps;
    }

    if (m_ModelWithStationaryWaterComponent)
    {
        m_DictionaryDirections.insert(m_DictionaryDirections.begin(),fakeIsotropicDirection);
        ++countIsoComps;
    }

    if (m_ModelWithRestrictedWaterComponent)
    {
        m_DictionaryDirections.insert(m_DictionaryDirections.begin(),fakeIsotropicDirection);
        ++countIsoComps;
    }

    if (m_ModelWithStaniszComponent)
    {
        m_DictionaryDirections.insert(m_DictionaryDirections.begin(),fakeIsotropicDirection);
        ++countIsoComps;
    }

    m_SparseSticksDictionary.set_size(m_NumberOfImages,countIsoComps + m_NumberOfDictionaryEntries);
    m_SparseSticksDictionary.fill(0.0);

    countIsoComps = 0;
    MCMPointer mcm;
    MCMCreatorType *mcmCreator = m_MCMCreators[0];
    mcmCreator->SetModelWithFreeWaterComponent(false);
    mcmCreator->SetModelWithStationaryWaterComponent(false);
    mcmCreator->SetModelWithRestrictedWaterComponent(false);
    mcmCreator->SetModelWithStaniszComponent(false);
    mcmCreator->SetNumberOfCompartments(0);

    if (m_ModelWithFreeWaterComponent)
    {
        mcmCreator->SetModelWithFreeWaterComponent(true);
        mcm = mcmCreator->GetNewMultiCompartmentModel();
        mcmCreator->SetModelWithFreeWaterComponent(false);

        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            m_SparseSticksDictionary(i,countIsoComps) = mcm->GetPredictedSignal(m_SmallDelta, m_BigDelta, m_GradientStrengths[i], m_GradientDirections[i]);

        ++countIsoComps;
    }

    if (m_ModelWithStationaryWaterComponent)
    {
        mcmCreator->SetModelWithStationaryWaterComponent(true);
        mcm = mcmCreator->GetNewMultiCompartmentModel();
        mcmCreator->SetModelWithStationaryWaterComponent(false);

        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            m_SparseSticksDictionary(i,countIsoComps) = mcm->GetPredictedSignal(m_SmallDelta, m_BigDelta, m_GradientStrengths[i], m_GradientDirections[i]);

        ++countIsoComps;
    }

    if (m_ModelWithRestrictedWaterComponent)
    {
        mcmCreator->SetModelWithRestrictedWaterComponent(true);
        mcm = mcmCreator->GetNewMultiCompartmentModel();
        mcmCreator->SetModelWithRestrictedWaterComponent(false);

        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            m_SparseSticksDictionary(i,countIsoComps) = mcm->GetPredictedSignal(m_SmallDelta, m_BigDelta, m_GradientStrengths[i], m_GradientDirections[i]);

        ++countIsoComps;
    }

    if (m_ModelWithStaniszComponent)
    {
        mcmCreator->SetModelWithStaniszComponent(true);
        mcm = mcmCreator->GetNewMultiCompartmentModel();
        mcmCreator->SetModelWithStaniszComponent(false);

        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            m_SparseSticksDictionary(i,countIsoComps) = mcm->GetPredictedSignal(m_SmallDelta, m_BigDelta, m_GradientStrengths[i], m_GradientDirections[i]);

        ++countIsoComps;
    }


    mcmCreator->SetNumberOfCompartments(1);
    mcmCreator->SetCompartmentType(Stick);

    mcm = mcmCreator->GetNewMultiCompartmentModel();

    for (unsigned int i = 0;i < m_NumberOfDictionaryEntries;++i)
    {
        anima::TransformCartesianToSphericalCoordinates(m_DictionaryDirections[i + countIsoComps],
                m_DictionaryDirections[i + countIsoComps]);

        mcm->GetCompartment(0)->SetOrientationTheta(m_DictionaryDirections[i + countIsoComps][0]);
        mcm->GetCompartment(0)->SetOrientationPhi(m_DictionaryDirections[i + countIsoComps][1]);

        for (unsigned int j = 0;j < m_NumberOfImages;++j)
            m_SparseSticksDictionary(j,countIsoComps + i) = mcm->GetPredictedSignal(m_SmallDelta, m_BigDelta, m_GradientStrengths[j], m_GradientDirections[j]);
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ComputeExtraAxonalAndKappaCoarseGrids()
{
    // m_ValuesCoarseGrid[0] -> extra axonal fractions, m_ValuesCoarseGrid[1] -> kappa
    m_ValuesCoarseGrid.resize(2);

    unsigned int coarseGridSize = 8;
    m_ValuesCoarseGrid[0].resize(coarseGridSize);
    m_ValuesCoarseGrid[1].resize(coarseGridSize);

    // The anisotropy index suggested by NODDI signal expression is not the ODI
    // index defined in Zhang et al., 2012, Neuroimage. Instead, it amounts to
    // (3 * tau1(k) - 1) / 2 \in [0,1] which is not analytically invertible.
    // However it is well approximated by k^power / (halfLife + k^power) where
    // power and halfLife are given by:

    double power = 1.385829;
    double halfLife = 5.312364;
    for (unsigned int i = 0;i < coarseGridSize;++i)
    {
        double tmpVal = (i + 1.0) / (coarseGridSize + 1.0);
        m_ValuesCoarseGrid[0][i] = tmpVal;
        m_ValuesCoarseGrid[1][i] = std::pow(tmpVal * halfLife / (1.0 - tmpVal), 1.0 / power);
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ComputeTensorRadialDiffsAndAzimuthCoarseGrids()
{
    // m_ValuesCoarseGrid[0] -> azimuth
    // m_ValuesCoarseGrid[1] -> radial diff 1 vs axial diff weight
    // m_ValuesCoarseGrid[2] -> radial diff 2 vs radial diff 1 weight
    m_ValuesCoarseGrid.resize(3);

    unsigned int coarseGridSize = 8;
    for (unsigned int i = 0;i < 3;++i)
        m_ValuesCoarseGrid[i].resize(coarseGridSize);

    for (unsigned int i = 0;i < coarseGridSize;++i)
    {
        m_ValuesCoarseGrid[0][i] = 2.0 * (i + 1) * M_PI / (coarseGridSize + 1.0);
        m_ValuesCoarseGrid[1][i] = i / (coarseGridSize - 1.0);
        m_ValuesCoarseGrid[2][i] = i / (coarseGridSize - 1.0);
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ThreadedGenerateData(const OutputImageRegionType &outputRegionForThread, itk::ThreadIdType threadId)
{
    typedef itk::ImageRegionConstIterator <InputImageType> ConstImageIteratorType;

    std::vector <ConstImageIteratorType> inIterators(m_NumberOfImages);
    for (unsigned int i = 0;i < m_NumberOfImages;++i)
        inIterators[i] = ConstImageIteratorType(this->GetInput(i),outputRegionForThread);

    typedef itk::ImageRegionIterator <OutputImageType> OutImageIteratorType;
    typedef itk::ImageRegionConstIterator <VectorImageType> VectorImageIteratorType;
    OutImageIteratorType outIterator(this->GetOutput(),outputRegionForThread);

    typedef itk::ImageRegionIterator <MaskImageType> MaskIteratorType;
    MaskIteratorType maskItr(this->GetComputationMask(),outputRegionForThread);

    typedef itk::ImageRegionIterator <OutputScalarImageType> ImageIteratorType;
    ImageIteratorType aiccIterator(m_AICcVolume, outputRegionForThread);
    ImageIteratorType b0Iterator(m_B0Volume, outputRegionForThread);
    ImageIteratorType sigmaIterator(m_SigmaSquareVolume, outputRegionForThread);

    typedef itk::ImageRegionIterator <MoseImageType> MoseIteratorType;
    MoseIteratorType moseIterator(m_MoseVolume, outputRegionForThread);

    std::vector <double> observedSignals(m_NumberOfImages,0);

    typename OutputImageType::PixelType resVec(this->GetOutput()->GetNumberOfComponentsPerPixel());

    MCMPointer mcmData = 0;
    MCMPointer outputMCMData = this->GetOutput()->GetDescriptionModel()->Clone();
    MCMType::ListType outputWeights(outputMCMData->GetNumberOfCompartments(),0);

    double aiccValue, b0Value, sigmaSqValue;

    while (!outIterator.IsAtEnd())
    {
        resVec.Fill(0.0);

        if (maskItr.Get() == 0)
        {
            outIterator.Set(resVec);

            for (unsigned int i = 0;i < m_NumberOfImages;++i)
                ++inIterators[i];

            ++outIterator;
            ++maskItr;
            ++aiccIterator;
            ++b0Iterator;
            ++sigmaIterator;
            ++moseIterator;

            continue;
        }

        // Load DWI
        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            observedSignals[i] = inIterators[i].Get();

        int moseValue = -1;
        bool estimateNonIsoCompartments = false;
        if (m_ExternalMoseVolume)
        {
            moseValue = moseIterator.Get();
            if (moseValue > 0)
                estimateNonIsoCompartments = true;
        }
        else if (m_NumberOfCompartments > 0)
            estimateNonIsoCompartments = true;

        bool hasIsoCompartment = m_ModelWithFreeWaterComponent || m_ModelWithRestrictedWaterComponent || m_ModelWithStationaryWaterComponent || m_ModelWithStaniszComponent;
        if (estimateNonIsoCompartments)
        {
            // If model selection, handle it here
            unsigned int minimalNumberOfCompartments = m_NumberOfCompartments;
            unsigned int maximalNumberOfCompartments = m_NumberOfCompartments;
            if (m_FindOptimalNumberOfCompartments)
            {
                minimalNumberOfCompartments = 1;
                moseValue = 0;

                if (hasIsoCompartment)
                    this->EstimateFreeWaterModel(mcmData,observedSignals,threadId,aiccValue,b0Value,sigmaSqValue);
            }
            else if (moseValue != -1)
            {
                int numMoseCompartments = std::min(moseValue,(int)m_NumberOfCompartments);
                minimalNumberOfCompartments = numMoseCompartments;
                maximalNumberOfCompartments = numMoseCompartments;
            }

            for (unsigned int i = minimalNumberOfCompartments;i <= maximalNumberOfCompartments;++i)
            {
                double tmpB0Value = 0;
                double tmpSigmaSqValue = 0;
                double tmpAiccValue = 0;
                MCMPointer mcmValue;

                this->OptimizeNonIsotropicCompartments(mcmValue,i,observedSignals,threadId,tmpAiccValue,tmpB0Value,tmpSigmaSqValue);

                if ((tmpAiccValue < aiccValue)||(!m_FindOptimalNumberOfCompartments))
                {
                    aiccValue = tmpAiccValue;
                    if (m_FindOptimalNumberOfCompartments)
                        moseValue = i;
                    b0Value = tmpB0Value;
                    sigmaSqValue = tmpSigmaSqValue;
                    mcmData = mcmValue;
                }
            }
        }
        else if (hasIsoCompartment)
            this->EstimateFreeWaterModel(mcmData,observedSignals,threadId,aiccValue,b0Value,sigmaSqValue);
        else
            itkExceptionMacro("Nothing to estimate...");

        if (outputMCMData->GetNumberOfCompartments() != mcmData->GetNumberOfCompartments())
        {
            // If we are selecting the number of compartments, create some empty ones here
            std::fill(outputWeights.begin(),outputWeights.end(),0.0);
            for (unsigned int i = 0;i < mcmData->GetNumberOfCompartments();++i)
            {
                outputMCMData->GetCompartment(i)->CopyFromOther(mcmData->GetCompartment(i));
                outputWeights[i] = mcmData->GetCompartmentWeight(i);
            }

            outputMCMData->SetCompartmentWeights(outputWeights);
            resVec = outputMCMData->GetModelVector();
        }
        else
            resVec = mcmData->GetModelVector();

        outIterator.Set(resVec);
        aiccIterator.Set(aiccValue);
        b0Iterator.Set(b0Value);
        sigmaIterator.Set(sigmaSqValue);
        moseIterator.Set(mcmData->GetNumberOfCompartments() - mcmData->GetNumberOfIsotropicCompartments());

        for (unsigned int i = 0;i < m_NumberOfImages;++i)
            ++inIterators[i];

        ++outIterator;
        ++maskItr;
        ++aiccIterator;
        ++b0Iterator;
        ++sigmaIterator;
        ++moseIterator;
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::OptimizeNonIsotropicCompartments(MCMPointer &mcmValue, unsigned int currentNumberOfCompartments,
                                   std::vector <double> &observedSignals, itk::ThreadIdType threadId,
                                   double &aiccValue, double &b0Value, double &sigmaSqValue)
{
    b0Value = 0;
    sigmaSqValue = 1;
    aiccValue = -1;

    this->InitialOrientationsEstimation(mcmValue,currentNumberOfCompartments,observedSignals,threadId,
                                        aiccValue,b0Value,sigmaSqValue);

    this->ModelEstimation(mcmValue,observedSignals,threadId,aiccValue,b0Value,sigmaSqValue);
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::EstimateFreeWaterModel(MCMPointer &mcmValue, std::vector <double> &observedSignals, itk::ThreadIdType threadId,
                         double &aiccValue, double &b0Value, double &sigmaSqValue)
{
    // Declarations for optimization
    MCMCreatorType *mcmCreator = m_MCMCreators[threadId];
    mcmCreator->SetModelWithFreeWaterComponent(m_ModelWithFreeWaterComponent);
    mcmCreator->SetModelWithStationaryWaterComponent(m_ModelWithStationaryWaterComponent);
    mcmCreator->SetModelWithRestrictedWaterComponent(m_ModelWithRestrictedWaterComponent);
    mcmCreator->SetModelWithStaniszComponent(m_ModelWithStaniszComponent);
    mcmCreator->SetNumberOfCompartments(0);
    mcmCreator->SetVariableProjectionEstimationMode(m_MLEstimationStrategy == VariableProjection);
    mcmCreator->SetUseConstrainedFreeWaterDiffusivity(m_UseConstrainedFreeWaterDiffusivity);
    mcmCreator->SetUseConstrainedIRWDiffusivity(m_UseConstrainedIRWDiffusivity);
    mcmCreator->SetUseConstrainedStaniszDiffusivity(m_UseConstrainedStaniszDiffusivity);
    mcmCreator->SetUseConstrainedStaniszRadius(m_UseConstrainedStaniszRadius);

    mcmValue = mcmCreator->GetNewMultiCompartmentModel();

    b0Value = 0;
    sigmaSqValue = 1;

    CostFunctionBasePointer cost = this->CreateCostFunction(observedSignals,mcmValue);

    unsigned int dimension = mcmValue->GetNumberOfParameters();
    ParametersType p(dimension);
    MCMType::ListType workVec(dimension);

    if (dimension > 0)
    {
        itk::Array<double> lowerBounds(dimension), upperBounds(dimension);

        workVec = mcmValue->GetParameterLowerBounds();
        for (unsigned int i = 0;i < dimension;++i)
            lowerBounds[i] = workVec[i];

        workVec = mcmValue->GetParameterUpperBounds();
        for (unsigned int i = 0;i < dimension;++i)
            upperBounds[i] = workVec[i];

        workVec = mcmValue->GetParametersAsVector();
        for (unsigned int i = 0;i < dimension;++i)
            p[i] = workVec[i];

        double costValue = this->PerformSingleOptimization(p,cost,lowerBounds,upperBounds);

        // - Get estimated DTI and B0
        for (unsigned int i = 0;i < dimension;++i)
            workVec[i] = p[i];

        mcmValue->SetParametersFromVector(workVec);

        this->GetProfiledInformation(cost,mcmValue,b0Value,sigmaSqValue);
        aiccValue = this->ComputeAICcValue(mcmValue,costValue);
    }
    else
    {
        double costValue = this->GetCostValue(cost,p);
        this->GetProfiledInformation(cost,mcmValue,b0Value,sigmaSqValue);

        aiccValue = this->ComputeAICcValue(mcmValue,costValue);
    }
}

template <class InputPixelType, class OutputPixelType>
typename MCMEstimatorImageFilter<InputPixelType, OutputPixelType>::CostFunctionBasePointer
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>::CreateCostFunction(std::vector <double> &observedSignals, MCMPointer &mcmModel)
{
    CostFunctionBasePointer returnCost;

    if (m_NoiseType != Gaussian)
        itkExceptionMacro("Cost function type not supported");

    anima::BaseMCMCost::Pointer baseCost;
    if (m_MLEstimationStrategy != VariableProjection)
    {
        anima::GaussianMCMCost::Pointer internalCost = anima::GaussianMCMCost::New();
        internalCost->SetMarginalEstimation(m_MLEstimationStrategy == Marginal);

        baseCost = internalCost;
    }
    else
    {
        anima::GaussianMCMVariableProjectionCost::Pointer internalCost = anima::GaussianMCMVariableProjectionCost::New();

        if (m_Optimizer == "levenberg")
            internalCost->SetUseDerivative(!m_VNLDerivativeComputation);
        else
            internalCost->SetUseDerivative((m_Optimizer == "ccsaq")||(m_Optimizer == "bfgs"));

        baseCost = internalCost;
    }

    baseCost->SetObservedSignals(observedSignals);
    baseCost->SetGradients(m_GradientDirections);
    baseCost->SetSmallDelta(m_SmallDelta);
    baseCost->SetBigDelta(m_BigDelta);
    baseCost->SetGradientStrengths(m_GradientStrengths);
    baseCost->SetMCMStructure(mcmModel);

    if (m_Optimizer == "levenberg")
    {
        anima::MCMMultipleValuedCostFunction::Pointer tmpCost =
                anima::MCMMultipleValuedCostFunction::New();

        tmpCost->SetInternalCost(baseCost);

        returnCost = tmpCost;
    }
    else
    {
        anima::MCMSingleValuedCostFunction::Pointer tmpCost =
                anima::MCMSingleValuedCostFunction::New();

        tmpCost->SetInternalCost(baseCost);

        returnCost = tmpCost;
    }

    return returnCost;
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::InitialOrientationsEstimation(MCMPointer &mcmValue, unsigned int currentNumberOfCompartments,
                                std::vector <double> &observedSignals, itk::ThreadIdType threadId,
                                double &aiccValue, double &b0Value, double &sigmaSqValue)
{
    b0Value = 0;
    sigmaSqValue = 1;
    aiccValue = -1;

    // Single DTI is already estimated, get it to use in next step
    // - First create output object
    MCMCreatorType *mcmCreator = m_MCMCreators[threadId];

    // - First create model
    mcmCreator->SetModelWithFreeWaterComponent(m_ModelWithFreeWaterComponent);
    mcmCreator->SetModelWithStationaryWaterComponent(m_ModelWithStationaryWaterComponent);
    mcmCreator->SetModelWithRestrictedWaterComponent(m_ModelWithRestrictedWaterComponent);
    mcmCreator->SetModelWithStaniszComponent(m_ModelWithStaniszComponent);
    mcmCreator->SetCompartmentType(Stick);
    mcmCreator->SetNumberOfCompartments(currentNumberOfCompartments);
    mcmCreator->SetVariableProjectionEstimationMode(m_MLEstimationStrategy == VariableProjection);
    mcmCreator->SetUseConstrainedDiffusivity(true);
    mcmCreator->SetUseConstrainedFreeWaterDiffusivity(m_UseConstrainedFreeWaterDiffusivity);
    mcmCreator->SetUseConstrainedIRWDiffusivity(m_UseConstrainedIRWDiffusivity);
    mcmCreator->SetUseConstrainedStaniszDiffusivity(m_UseConstrainedStaniszDiffusivity);
    mcmCreator->SetUseConstrainedStaniszRadius(m_UseConstrainedStaniszRadius);
    mcmCreator->SetUseCommonDiffusivities(m_UseCommonDiffusivities);

    MCMPointer mcmUpdateValue = mcmCreator->GetNewMultiCompartmentModel();

    // - Now initialize sticks from dictionary
    this->SparseInitializeSticks(mcmUpdateValue,observedSignals,threadId);

    unsigned int dimension = mcmUpdateValue->GetNumberOfParameters();
    ParametersType p(dimension);
    MCMType::ListType workVec(dimension);
    itk::Array<double> lowerBounds(dimension), upperBounds(dimension);

    workVec = mcmUpdateValue->GetParameterLowerBounds();
    for (unsigned int j = 0;j < dimension;++j)
        lowerBounds[j] = workVec[j];

    workVec = mcmUpdateValue->GetParameterUpperBounds();
    for (unsigned int j = 0;j < dimension;++j)
        upperBounds[j] = workVec[j];

    CostFunctionBasePointer cost = this->CreateCostFunction(observedSignals,mcmUpdateValue);

    // - Update ball and stick model against observed signals
    workVec = mcmUpdateValue->GetParametersAsVector();
    for (unsigned int j = 0;j < dimension;++j)
        p[j] = workVec[j];

    double costValue = this->PerformSingleOptimization(p,cost,lowerBounds,upperBounds);

    // - Get estimated data
    for (unsigned int j = 0;j < dimension;++j)
        workVec[j] = p[j];

    mcmUpdateValue->SetParametersFromVector(workVec);

    this->GetProfiledInformation(cost,mcmUpdateValue,b0Value,sigmaSqValue);

    aiccValue = this->ComputeAICcValue(mcmUpdateValue,costValue);
    mcmValue = mcmUpdateValue;
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ModelEstimation(MCMPointer &mcmValue, std::vector <double> &observedSignals, itk::ThreadIdType threadId,
                  double &aiccValue, double &b0Value, double &sigmaSqValue)
{
    unsigned int optimalNumberOfCompartments = mcmValue->GetNumberOfCompartments() - mcmValue->GetNumberOfIsotropicCompartments();

    //Already done in initial orientations estimation
    if ((m_CompartmentType == Stick)&&(m_UseConstrainedDiffusivity))
        return;

    // - First create model
    MCMCreatorType *mcmCreator = m_MCMCreators[threadId];
    mcmCreator->SetCompartmentType(Stick);
    mcmCreator->SetNumberOfCompartments(optimalNumberOfCompartments);
    mcmCreator->SetUseConstrainedDiffusivity(m_UseConstrainedDiffusivity);

    MCMPointer mcmUpdateValue = mcmCreator->GetNewMultiCompartmentModel();

    // - Now the tricky part: initialize from previous model, handled somewhere else
    this->InitializeModelFromSimplifiedOne(mcmValue,mcmUpdateValue);

    CostFunctionBasePointer cost = this->CreateCostFunction(observedSignals,mcmUpdateValue);

    unsigned int dimension = mcmUpdateValue->GetNumberOfParameters();
    ParametersType p(dimension);
    MCMType::ListType workVec(dimension);
    itk::Array<double> lowerBounds(dimension), upperBounds(dimension);

    workVec = mcmUpdateValue->GetParameterLowerBounds();
    for (unsigned int i = 0;i < dimension;++i)
        lowerBounds[i] = workVec[i];

    workVec = mcmUpdateValue->GetParameterUpperBounds();
    for (unsigned int i = 0;i < dimension;++i)
        upperBounds[i] = workVec[i];

    workVec = mcmUpdateValue->GetParametersAsVector();
    for (unsigned int i = 0;i < dimension;++i)
        p[i] = workVec[i];

    double costValue = this->PerformSingleOptimization(p,cost,lowerBounds,upperBounds);
    this->GetProfiledInformation(cost,mcmUpdateValue,b0Value,sigmaSqValue);
    
    for (unsigned int i = 0;i < dimension;++i)
        workVec[i] = p[i];

    mcmUpdateValue->SetParametersFromVector(workVec);

    mcmValue = mcmUpdateValue;
    aiccValue = this->ComputeAICcValue(mcmValue,costValue);

    if (m_CompartmentType == Stick)
        return;

    // We're done with ball and stick, next up is ball and zeppelin
    // - First create model
    mcmCreator->SetCompartmentType(Zeppelin);
    mcmUpdateValue = mcmCreator->GetNewMultiCompartmentModel();

    // - Now the tricky part: initialize from previous model, handled somewhere else
    this->InitializeModelFromSimplifiedOne(mcmValue,mcmUpdateValue);

    // - Update ball and zeppelin model against observed signals
    cost = this->CreateCostFunction(observedSignals,mcmUpdateValue);
    dimension = mcmUpdateValue->GetNumberOfParameters();
    p.SetSize(dimension);
    lowerBounds.SetSize(dimension);
    upperBounds.SetSize(dimension);

    workVec = mcmUpdateValue->GetParameterLowerBounds();
    for (unsigned int i = 0;i < dimension;++i)
        lowerBounds[i] = workVec[i];

    workVec = mcmUpdateValue->GetParameterUpperBounds();
    for (unsigned int i = 0;i < dimension;++i)
        upperBounds[i] = workVec[i];

    workVec = mcmUpdateValue->GetParametersAsVector();
    for (unsigned int i = 0;i < dimension;++i)
        p[i] = workVec[i];
    
    costValue = this->PerformSingleOptimization(p,cost,lowerBounds,upperBounds);
    this->GetProfiledInformation(cost,mcmUpdateValue,b0Value,sigmaSqValue);
    
    for (unsigned int i = 0;i < dimension;++i)
        workVec[i] = p[i];

    mcmUpdateValue->SetParametersFromVector(workVec);
    
    mcmValue = mcmUpdateValue;
    aiccValue = this->ComputeAICcValue(mcmValue,costValue);

    if (m_CompartmentType == Zeppelin)
        return;

    // Finally, we're done with ball and zeppelin, an example of what's next up with multi-tensor
    // - First create model
    mcmCreator->SetCompartmentType(m_CompartmentType);
    mcmCreator->SetUseConstrainedExtraAxonalFraction(m_UseConstrainedExtraAxonalFraction);
    mcmCreator->SetUseConstrainedOrientationConcentration(m_UseConstrainedOrientationConcentration);
    mcmCreator->SetUseCommonConcentrations(m_UseCommonConcentrations);
    mcmCreator->SetUseCommonExtraAxonalFractions(m_UseCommonExtraAxonalFractions);

    mcmUpdateValue = mcmCreator->GetNewMultiCompartmentModel();

    // - Now the tricky part: initialize from previous model, handled somewhere else
    this->InitializeModelFromSimplifiedOne(mcmValue,mcmUpdateValue);

    // - Update complex model against observed signals
    cost = this->CreateCostFunction(observedSignals,mcmUpdateValue);
    dimension = mcmUpdateValue->GetNumberOfParameters();
    p.SetSize(dimension);
    lowerBounds.SetSize(dimension);
    upperBounds.SetSize(dimension);

    workVec = mcmUpdateValue->GetParameterLowerBounds();
    for (unsigned int i = 0;i < dimension;++i)
        lowerBounds[i] = workVec[i];

    workVec = mcmUpdateValue->GetParameterUpperBounds();
    for (unsigned int i = 0;i < dimension;++i)
        upperBounds[i] = workVec[i];
    
    switch (m_CompartmentType)
    {
        case NODDI:
        case DDI:
            this->ExtraAxonalAndKappaCoarseGridInitialization(mcmUpdateValue, cost, workVec, p);
            break;

        case Tensor:
            this->TensorCoarseGridInitialization(mcmUpdateValue, cost, workVec, p);
            break;

        default:
            itkExceptionMacro("No coarse grid initialization for simple models, shouldn't be here");
            break;
    }

    workVec = mcmUpdateValue->GetParametersAsVector();
    for (unsigned int i = 0;i < dimension;++i)
        p[i] = workVec[i];

    costValue = this->PerformSingleOptimization(p,cost,lowerBounds,upperBounds);
    this->GetProfiledInformation(cost,mcmUpdateValue,b0Value,sigmaSqValue);

    for (unsigned int i = 0;i < dimension;++i)
        workVec[i] = p[i];

    mcmUpdateValue->SetParametersFromVector(workVec);

    mcmValue = mcmUpdateValue;
    aiccValue = this->ComputeAICcValue(mcmValue,costValue);
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ExtraAxonalAndKappaCoarseGridInitialization(MCMPointer &mcmUpdateValue, CostFunctionBasePointer &cost,
                                              MCMType::ListType &workVec,ParametersType &p)
{
    unsigned int dimension = p.GetSize();
    double optNu = 0;
    double optKappa = 0;
    double optValue = -1.0;

    unsigned int numIsoCompartments = mcmUpdateValue->GetNumberOfIsotropicCompartments();
    unsigned int numCompartments = mcmUpdateValue->GetNumberOfCompartments();

    for (unsigned int j = 0;j < m_ValuesCoarseGrid[1].size();++j)
    {
        double tmpKappa = m_ValuesCoarseGrid[1][j];

        for (unsigned int k = 0;k < m_ValuesCoarseGrid[0].size();++k)
        {
            double tmpNu = m_ValuesCoarseGrid[0][k];

            for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
            {
                mcmUpdateValue->GetCompartment(i)->SetOrientationConcentration(tmpKappa);
                mcmUpdateValue->GetCompartment(i)->SetExtraAxonalFraction(tmpNu);
            }

            workVec = mcmUpdateValue->GetParametersAsVector();
            for (unsigned int i = 0;i < dimension;++i)
                p[i] = workVec[i];

            double tmpValue = this->GetCostValue(cost,p);

            if ((tmpValue < optValue) || (optValue < 0))
            {
                optValue = tmpValue;
                optKappa = tmpKappa;
                optNu = tmpNu;
            }
        }
    }

    for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
    {
        mcmUpdateValue->GetCompartment(i)->SetOrientationConcentration(optKappa);
        mcmUpdateValue->GetCompartment(i)->SetExtraAxonalFraction(optNu);
    }
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::TensorCoarseGridInitialization(MCMPointer &mcmUpdateValue, CostFunctionBasePointer &cost,
                                 MCMType::ListType &workVec,ParametersType &p)
{
    unsigned int dimension = p.GetSize();
    unsigned int numIsoCompartments = mcmUpdateValue->GetNumberOfIsotropicCompartments();
    unsigned int numCompartments = mcmUpdateValue->GetNumberOfCompartments();

    double optRadialDiff1 = 0.0;
    double optRadialDiff2 = 0.0;
    double optAzimuth = 0.0;
    double optValue = - 1.0;

    double meanAxialDiff = 0.0;
    double meanRadialDiff = 0.0;
    unsigned int numUsefulCompartments = 0;

    for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
    {
        if (mcmUpdateValue->GetCompartmentWeight(i) > 0)
        {
            meanAxialDiff += mcmUpdateValue->GetCompartment(i)->GetAxialDiffusivity();
            meanRadialDiff += mcmUpdateValue->GetCompartment(i)->GetRadialDiffusivity1();

            ++numUsefulCompartments;
        }
    }

    if (numUsefulCompartments > 0)
    {
        meanAxialDiff /= numUsefulCompartments;
        meanRadialDiff /= numUsefulCompartments;
    }
    else
    {
        meanAxialDiff = m_AxialDiffusivityValue;
        meanRadialDiff = (m_RadialDiffusivity1Value + m_RadialDiffusivity2Value) / 2.0;
    }

    if (meanAxialDiff - meanRadialDiff < anima::MCMAxialDiffusivityAddonLowerBound)
        meanAxialDiff = meanRadialDiff + anima::MCMAxialDiffusivityAddonLowerBound;

    for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
        mcmUpdateValue->GetCompartment(i)->SetAxialDiffusivity(meanAxialDiff);

    for (unsigned int l = 0;l < m_ValuesCoarseGrid[2].size();++l)
    {
        double tmpWeight2 = m_ValuesCoarseGrid[2][l];

        // In between meanRD and (meanRD + MCMDiffusivityLowerBound) / 2
        double tmpRadialDiffusivity2 = 0.5 * ((1.0 + tmpWeight2) * meanRadialDiff + (1.0 - tmpWeight2) * anima::MCMDiffusivityLowerBound);

        for (unsigned int k = 0;k < m_ValuesCoarseGrid[1].size();++k)
        {
            double tmpWeight1 = m_ValuesCoarseGrid[1][k];

            // In between meanRD and (meanRD + meanAD) / 2
            double tmpRadialDiffusivity1 = 0.5 * ((1.0 + tmpWeight1) * meanRadialDiff + (1.0 - tmpWeight1) * meanAxialDiff);
            if (meanAxialDiff - tmpRadialDiffusivity1 < anima::MCMAxialDiffusivityAddonLowerBound)
                continue;

            for (unsigned int j = 0;j < m_ValuesCoarseGrid[0].size();++j)
            {
                double tmpAzimuth = m_ValuesCoarseGrid[0][j];

                for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
                {
                    mcmUpdateValue->GetCompartment(i)->SetPerpendicularAngle(tmpAzimuth);
                    mcmUpdateValue->GetCompartment(i)->SetRadialDiffusivity1(tmpRadialDiffusivity1);
                    mcmUpdateValue->GetCompartment(i)->SetRadialDiffusivity2(tmpRadialDiffusivity2);
                }

                workVec = mcmUpdateValue->GetParametersAsVector();
                for (unsigned int i = 0;i < dimension;++i)
                    p[i] = workVec[i];

                double tmpValue = this->GetCostValue(cost,p);

                if ((tmpValue < optValue) || (optValue < 0))
                {
                    optValue = tmpValue;
                    optRadialDiff1 = tmpRadialDiffusivity1;
                    optRadialDiff2 = tmpRadialDiffusivity2;
                    optAzimuth = tmpAzimuth;
                }
            }
        }
    }

    for (unsigned int i = numIsoCompartments;i < numCompartments;++i)
    {
        mcmUpdateValue->GetCompartment(i)->SetPerpendicularAngle(optAzimuth);
        mcmUpdateValue->GetCompartment(i)->SetRadialDiffusivity1(optRadialDiff1);
        mcmUpdateValue->GetCompartment(i)->SetRadialDiffusivity2(optRadialDiff2);
    }
}

template <class InputPixelType, class OutputPixelType>
typename MCMEstimatorImageFilter<InputPixelType, OutputPixelType>::OptimizerPointer
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::CreateOptimizer(CostFunctionBasePointer &cost, itk::Array<double> &lowerBounds, itk::Array<double> &upperBounds)
{
    OptimizerPointer returnOpt;
    double xTol = m_XTolerance;
    bool defaultTol = false;
    if (m_XTolerance == 0)
    {
        defaultTol = true;
        xTol = 1.0e-4;
    }

    double maxEvals = m_MaxEval;
    if (m_MaxEval == 0)
        maxEvals = 400 * lowerBounds.GetSize();

    if (m_Optimizer != "levenberg")
    {
        anima::NLOPTOptimizers::Pointer tmpOpt = anima::NLOPTOptimizers::New();

        if (m_MLEstimationStrategy != VariableProjection)
        {
            anima::MCMSingleValuedCostFunction *costCast =
                    dynamic_cast <anima::MCMSingleValuedCostFunction *> (cost.GetPointer());
            tmpOpt->SetCostFunction(costCast);
            tmpOpt->SetAlgorithm(NLOPT_AUGLAG);

            if (m_Optimizer == "bobyqa")
            {
                tmpOpt->SetLocalOptimizer(NLOPT_LN_BOBYQA);
                if (defaultTol)
                    xTol = 1.0e-7;
            }
            else if (m_Optimizer == "ccsaq")
                tmpOpt->SetLocalOptimizer(NLOPT_LD_CCSAQ);
            else if (m_Optimizer == "bfgs")
                tmpOpt->SetLocalOptimizer(NLOPT_LD_LBFGS);

            typedef anima::MCMWeightsInequalityConstraintFunction WeightInequalityFunctionType;
            WeightInequalityFunctionType::Pointer weightsInequality = WeightInequalityFunctionType::New();
            double wIneqTol = std::min(std::numeric_limits<double>::epsilon(), xTol / 10.0);
            weightsInequality->SetTolerance(wIneqTol);
            weightsInequality->SetMCMStructure(costCast->GetMCMStructure());

            tmpOpt->AddInequalityConstraint(weightsInequality);
        }
        else
        {
            if (m_Optimizer == "bobyqa")
            {
                tmpOpt->SetAlgorithm(NLOPT_LN_BOBYQA);
                if (defaultTol)
                    xTol = 1.0e-7;
            }
            else if (m_Optimizer == "ccsaq")
                tmpOpt->SetAlgorithm(NLOPT_LD_CCSAQ);
            else if (m_Optimizer == "bfgs")
                tmpOpt->SetAlgorithm(NLOPT_LD_LBFGS);

            anima::MCMSingleValuedCostFunction *costCast =
                    dynamic_cast <anima::MCMSingleValuedCostFunction *> (cost.GetPointer());
            tmpOpt->SetCostFunction(costCast);
        }

        tmpOpt->SetMaximize(false);
        tmpOpt->SetXTolRel(xTol);
        tmpOpt->SetFTolRel(xTol * 1.0e-2);
        tmpOpt->SetMaxEval(maxEvals);
        tmpOpt->SetVectorStorageSize(2000);
        
        if (!m_UseBoundedOptimization)
        {
            tmpOpt->SetLowerBoundParameters(lowerBounds);
            tmpOpt->SetUpperBoundParameters(upperBounds);
        }

        returnOpt = tmpOpt;
    }
    else
    {
        if (m_MLEstimationStrategy == Marginal)
            itkExceptionMacro("Levenberg Marquardt optimizer not supported with marginal optimization");

        typedef itk::LevenbergMarquardtOptimizer LevenbergMarquardtOptimizerType;
        LevenbergMarquardtOptimizerType::Pointer tmpOpt = LevenbergMarquardtOptimizerType::New();

        anima::MCMMultipleValuedCostFunction *costCast =
                dynamic_cast <anima::MCMMultipleValuedCostFunction *> (cost.GetPointer());
        
        double gTol = m_GTolerance;
        if (m_GTolerance == 0)
            gTol = 1.0e-5;
        
        tmpOpt->SetCostFunction(costCast);
        tmpOpt->SetEpsilonFunction(xTol * 1.0e-3);
        tmpOpt->SetGradientTolerance(gTol);
        tmpOpt->SetNumberOfIterations(maxEvals);
        tmpOpt->SetValueTolerance(xTol);
        tmpOpt->SetUseCostFunctionGradient(!m_VNLDerivativeComputation);
        
        returnOpt = tmpOpt;
    }

    return returnOpt;
}

template <class InputPixelType, class OutputPixelType>
double
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::PerformSingleOptimization(ParametersType &p, CostFunctionBasePointer &cost, itk::Array<double> &lowerBounds, itk::Array<double> &upperBounds)
{
    double costValue = this->GetCostValue(cost,p);
    
    OptimizerPointer optimizer = this->CreateOptimizer(cost,lowerBounds,upperBounds);
    
    optimizer->SetInitialPosition(p);
    optimizer->StartOptimization();

    p = optimizer->GetCurrentPosition();

    if (!m_UseBoundedOptimization)
    {
        // Takes care of round-off errors in NLOpt resulting
        // in parameters sometimes slightly off bounds
        for (unsigned int i = 0;i < p.GetSize();++i)
        {
            if (p[i] < lowerBounds[i])
                p[i] = lowerBounds[i];

            if (p[i] > upperBounds[i])
                p[i] = upperBounds[i];
        }
    }

    costValue = this->GetCostValue(cost,p);
    
    return costValue;
}

template <class InputPixelType, class OutputPixelType>
double
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::GetCostValue(CostFunctionBasePointer &cost, ParametersType &p)
{
    if (m_Optimizer == "levenberg")
    {
        anima::MCMMultipleValuedCostFunction *costCast =
                dynamic_cast <anima::MCMMultipleValuedCostFunction *> (cost.GetPointer());

        costCast->GetInternalCost()->GetValues(p);
        return costCast->GetInternalCost()->GetCurrentCostValue();
    }

    anima::MCMSingleValuedCostFunction *costCast =
            dynamic_cast <anima::MCMSingleValuedCostFunction *> (cost.GetPointer());

    return costCast->GetValue(p);
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::GetProfiledInformation(CostFunctionBasePointer &cost, MCMPointer &mcm, double &b0Value, double &sigmaSqValue)
{
    if (m_Optimizer == "levenberg")
    {
        anima::MCMMultipleValuedCostFunction *costCast =
                dynamic_cast <anima::MCMMultipleValuedCostFunction *> (cost.GetPointer());

        b0Value = costCast->GetB0Value();
        sigmaSqValue = costCast->GetSigmaSquare();

        if (m_MLEstimationStrategy == VariableProjection)
            mcm->SetCompartmentWeights(costCast->GetMCMStructure()->GetCompartmentWeights());
    }
    else
    {
        anima::MCMSingleValuedCostFunction *costCast =
                dynamic_cast <anima::MCMSingleValuedCostFunction *> (cost.GetPointer());

        b0Value = costCast->GetB0Value();
        sigmaSqValue = costCast->GetSigmaSquare();

        if (m_MLEstimationStrategy == VariableProjection)
            mcm->SetCompartmentWeights(costCast->GetMCMStructure()->GetCompartmentWeights());
    }
}

template <class InputPixelType, class OutputPixelType>
double
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::ComputeAICcValue(MCMPointer &mcmValue, double costValue)
{
    // Compute AICu (improvement over AICc)
    // nbparams is the number of parameters of the model plus the B0 value and the variance value
    double nbparams = mcmValue->GetNumberOfParameters() + 2.0;

    // We assume the cost value is returned as - 2 * log-likelihood
    double AICc = costValue + 2.0 * nbparams + 2.0 * nbparams * (nbparams + 1.0) / (m_NumberOfImages - nbparams - 1.0)
            + m_NumberOfImages * std::log(m_NumberOfImages / (m_NumberOfImages - nbparams - 1.0));

    return AICc;
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::SparseInitializeSticks(MCMPointer &complexModel, std::vector<double> &observedSignals, itk::ThreadIdType threadId)
{
    unsigned int numIsotropicComponents = m_ModelWithFreeWaterComponent + m_ModelWithRestrictedWaterComponent
            + m_ModelWithStaniszComponent + m_ModelWithStationaryWaterComponent;
    unsigned int numNonIsotropicComponents = complexModel->GetNumberOfCompartments() - numIsotropicComponents;

    anima::NNOrthogonalMatchingPursuitOptimizer::Pointer sparseOptimizer = anima::NNOrthogonalMatchingPursuitOptimizer::New();
    sparseOptimizer->SetDataMatrix(m_SparseSticksDictionary);

    ParametersType rightHandValues(observedSignals.size());
    for (unsigned int i = 0;i < observedSignals.size();++i)
        rightHandValues[i] = observedSignals[i];

    sparseOptimizer->SetPoints(rightHandValues);
    sparseOptimizer->SetMaximalNumberOfWeights(numNonIsotropicComponents);
    sparseOptimizer->SetIgnoredIndexesUpperBound(numIsotropicComponents);

    sparseOptimizer->StartOptimization();

    MCMType::ListType sparseWeights(complexModel->GetNumberOfCompartments(),0.0);
    ParametersType dictionaryWeights = sparseOptimizer->GetCurrentPosition();

    for (unsigned int i = 0;i < numIsotropicComponents;++i)
        sparseWeights[i] = dictionaryWeights[i];

    unsigned int numRealAnisotropicCompartments = 0;
    for (unsigned int i = numIsotropicComponents;i < dictionaryWeights.size();++i)
    {
        if (dictionaryWeights[i] > 0)
            ++numRealAnisotropicCompartments;
    }

    if (numRealAnisotropicCompartments != numNonIsotropicComponents)
    {
        MCMCreatorType *mcmCreator = m_MCMCreators[threadId];
        mcmCreator->SetNumberOfCompartments(numRealAnisotropicCompartments);
        complexModel = mcmCreator->GetNewMultiCompartmentModel();
        sparseWeights.resize(complexModel->GetNumberOfCompartments());
    }

    unsigned int pos = numIsotropicComponents;
    for (unsigned int i = numIsotropicComponents;i < dictionaryWeights.size();++i)
    {
        if (dictionaryWeights[i] > 0)
        {
            sparseWeights[pos] = dictionaryWeights[i];

            anima::BaseCompartment *currentCompartment = complexModel->GetCompartment(pos);
            currentCompartment->SetOrientationTheta(m_DictionaryDirections[i][0]);
            currentCompartment->SetOrientationPhi(m_DictionaryDirections[i][1]);

            ++pos;
        }
    }

    double sumWeights = 0;
    for (unsigned int i = 0;i < complexModel->GetNumberOfCompartments();++i)
        sumWeights += sparseWeights[i];

    for (unsigned int i = 0;i < complexModel->GetNumberOfCompartments();++i)
        sparseWeights[i] /= sumWeights;

    complexModel->SetCompartmentWeights(sparseWeights);
}

template <class InputPixelType, class OutputPixelType>
void
MCMEstimatorImageFilter<InputPixelType, OutputPixelType>
::InitializeModelFromSimplifiedOne(MCMPointer &simplifiedModel, MCMPointer &complexModel)
{
    // copy everything
    unsigned int numCompartments = complexModel->GetNumberOfCompartments();
    if (numCompartments != simplifiedModel->GetNumberOfCompartments())
        itkExceptionMacro("Simplified and complex model should have the same number of components.");

    for (unsigned int i = 0;i < numCompartments;++i)
        complexModel->GetCompartment(i)->CopyFromOther(simplifiedModel->GetCompartment(i));

    // Now we're talking about weights
    complexModel->SetCompartmentWeights(simplifiedModel->GetCompartmentWeights());
}

} // end namespace anima
