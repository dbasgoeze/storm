

#include "storm/utility/initialize.h"

#include "storm/settings/modules/GeneralSettings.h"
#include "storm/settings/modules/DebugSettings.h"
#include "storm-pomdp-cli/settings/modules/POMDPSettings.h"
#include "storm-pomdp-cli/settings/modules/GridApproximationSettings.h"
#include "storm-pomdp-cli/settings/PomdpSettings.h"

#include "storm/analysis/GraphConditions.h"

#include "storm-cli-utilities/cli.h"
#include "storm-cli-utilities/model-handling.h"

#include "storm-pomdp/transformer/KnownProbabilityTransformer.h"
#include "storm-pomdp/transformer/ApplyFiniteSchedulerToPomdp.h"
#include "storm-pomdp/transformer/GlobalPOMDPSelfLoopEliminator.h"
#include "storm-pomdp/transformer/GlobalPomdpMecChoiceEliminator.h"
#include "storm-pomdp/transformer/PomdpMemoryUnfolder.h"
#include "storm-pomdp/transformer/BinaryPomdpTransformer.h"
#include "storm-pomdp/transformer/MakePOMDPCanonic.h"
#include "storm-pomdp/analysis/UniqueObservationStates.h"
#include "storm-pomdp/analysis/QualitativeAnalysis.h"
#include "storm-pomdp/modelchecker/ApproximatePOMDPModelchecker.h"
#include "storm-pomdp/analysis/FormulaInformation.h"
#include "storm-pomdp/analysis/MemlessStrategySearchQualitative.h"
#include "storm-pomdp/analysis/QualitativeStrategySearchNaive.h"

#include "storm/api/storm.h"
#include "storm/modelchecker/results/ExplicitQuantitativeCheckResult.h"
#include "storm/modelchecker/results/ExplicitQualitativeCheckResult.h"
#include "storm/utility/NumberTraits.h"
#include "storm/utility/Stopwatch.h"
#include "storm/utility/SignalHandler.h"
#include "storm/utility/NumberTraits.h"

#include "storm/exceptions/UnexpectedException.h"
#include "storm/exceptions/NotSupportedException.h"

#include <typeinfo>

namespace storm {
    namespace pomdp {
        namespace cli {
            
            /// Perform preprocessings based on the graph structure (if requested or necessary). Return true, if some preprocessing has been done
            template<typename ValueType, storm::dd::DdType DdType>
            bool performPreprocessing(std::shared_ptr<storm::models::sparse::Pomdp<ValueType>>& pomdp, storm::pomdp::analysis::FormulaInformation& formulaInfo, storm::logic::Formula const& formula) {
                auto const& pomdpSettings = storm::settings::getModule<storm::settings::modules::POMDPSettings>();
                bool preprocessingPerformed = false;
                if (pomdpSettings.isSelfloopReductionSet()) {
                    bool apply = formulaInfo.isNonNestedReachabilityProbability() && formulaInfo.maximize();
                    apply = apply || (formulaInfo.isNonNestedExpectedRewardFormula() && formulaInfo.minimize());
                    if (apply) {
                        STORM_PRINT_AND_LOG("Eliminating self-loop choices ...");
                        uint64_t oldChoiceCount = pomdp->getNumberOfChoices();
                        storm::transformer::GlobalPOMDPSelfLoopEliminator<ValueType> selfLoopEliminator(*pomdp);
                        pomdp = selfLoopEliminator.transform();
                        STORM_PRINT_AND_LOG(oldChoiceCount - pomdp->getNumberOfChoices() << " choices eliminated through self-loop elimination." << std::endl);
                        preprocessingPerformed = true;
                    }
                }
                if (pomdpSettings.isQualitativeReductionSet() && formulaInfo.isNonNestedReachabilityProbability()) {
                    storm::analysis::QualitativeAnalysis<ValueType> qualitativeAnalysis(*pomdp);
                    STORM_PRINT_AND_LOG("Computing states with probability 0 ...");
                    storm::storage::BitVector prob0States = qualitativeAnalysis.analyseProb0(formula.asProbabilityOperatorFormula());
                    std::cout << prob0States << std::endl;
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    STORM_PRINT_AND_LOG("Computing states with probability 1 ...");
                    storm::storage::BitVector  prob1States = qualitativeAnalysis.analyseProb1(formula.asProbabilityOperatorFormula());
                    std::cout << prob1States << std::endl;
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    storm::pomdp::transformer::KnownProbabilityTransformer<ValueType> kpt = storm::pomdp::transformer::KnownProbabilityTransformer<ValueType>();
                    pomdp = kpt.transform(*pomdp, prob0States, prob1States);
                    // Update formulaInfo to changes from Preprocessing
                    formulaInfo.updateTargetStates(*pomdp, std::move(prob1States));
                    formulaInfo.updateSinkStates(*pomdp, std::move(prob0States));
                    preprocessingPerformed = true;
                }
                return preprocessingPerformed;
            }
            
            template<typename ValueType>
            void printResult(ValueType const& lowerBound, ValueType const& upperBound) {
                if (lowerBound == upperBound) {
                    STORM_PRINT_AND_LOG(lowerBound);
                } else {
                    STORM_PRINT_AND_LOG("[" << lowerBound << ", " << upperBound << "] (width=" << ValueType(upperBound - lowerBound) << ")");
                }
                if (storm::NumberTraits<ValueType>::IsExact) {
                    STORM_PRINT_AND_LOG(" (approx. ");
                    printResult(storm::utility::convertNumber<double>(lowerBound), storm::utility::convertNumber<double>(upperBound));
                    STORM_PRINT_AND_LOG(")");
                }
            }
            
            template<typename ValueType, storm::dd::DdType DdType>
            bool performAnalysis(std::shared_ptr<storm::models::sparse::Pomdp<ValueType>> const& pomdp, storm::pomdp::analysis::FormulaInformation const& formulaInfo, storm::logic::Formula const& formula) {
                auto const& pomdpSettings = storm::settings::getModule<storm::settings::modules::POMDPSettings>();
                bool analysisPerformed = false;
                if (pomdpSettings.isGridApproximationSet()) {
                    STORM_PRINT_AND_LOG("Applying grid approximation... ");
                    auto const& gridSettings = storm::settings::getModule<storm::settings::modules::GridApproximationSettings>();
                    typename storm::pomdp::modelchecker::ApproximatePOMDPModelchecker<storm::models::sparse::Pomdp<ValueType>>::Options options;
                    options.initialGridResolution = gridSettings.getGridResolution();
                    options.explorationThreshold = storm::utility::convertNumber<ValueType>(gridSettings.getExplorationThreshold());
                    options.doRefinement = gridSettings.isRefineSet();
                    options.refinementPrecision = storm::utility::convertNumber<ValueType>(gridSettings.getRefinementPrecision());
                    options.numericPrecision = storm::utility::convertNumber<ValueType>(gridSettings.getNumericPrecision());
                    options.cacheSubsimplices = gridSettings.isCacheSimplicesSet();
                    if (gridSettings.isUnfoldBeliefMdpSizeThresholdSet()) {
                        options.beliefMdpSizeThreshold = gridSettings.getUnfoldBeliefMdpSizeThreshold();
                    }
                    if (storm::NumberTraits<ValueType>::IsExact) {
                        if (gridSettings.isNumericPrecisionSetFromDefault()) {
                            STORM_LOG_WARN_COND(storm::utility::isZero(options.numericPrecision), "Setting numeric precision to zero because exact arithmethic is used.");
                            options.numericPrecision = storm::utility::zero<ValueType>();
                        } else {
                            STORM_LOG_WARN_COND(storm::utility::isZero(options.numericPrecision), "A non-zero numeric precision was set although exact arithmethic is used. Results might be inexact.");
                        }
                    }
                    storm::pomdp::modelchecker::ApproximatePOMDPModelchecker<storm::models::sparse::Pomdp<ValueType>> checker(*pomdp, options);
                    auto result = checker.check(formula);
                    checker.printStatisticsToStream(std::cout);
                    if (storm::utility::resources::isTerminate()) {
                        STORM_PRINT_AND_LOG("\nResult till abort: ")
                    } else {
                        STORM_PRINT_AND_LOG("\nResult: ")
                    }
                    printResult(result.lowerBound, result.upperBound);
                    STORM_PRINT_AND_LOG(std::endl);
                    analysisPerformed = true;
                }
                if (pomdpSettings.isMemlessSearchSet()) {
                    STORM_LOG_THROW(formulaInfo.isNonNestedReachabilityProbability(), storm::exceptions::NotSupportedException, "Qualitative memoryless scheduler search is not implemented for this property type.");

    //                    std::cout << std::endl;
    //                    pomdp->writeDotToStream(std::cout);
    //                    std::cout << std::endl;
    //                    std::cout << std::endl;
                    storm::expressions::ExpressionManager expressionManager;
                    std::shared_ptr<storm::utility::solver::SmtSolverFactory> smtSolverFactory = std::make_shared<storm::utility::solver::Z3SmtSolverFactory>();
                    if (pomdpSettings.getMemlessSearchMethod() == "ccd16memless") {
                        storm::pomdp::QualitativeStrategySearchNaive<ValueType> memlessSearch(*pomdp, formulaInfo.getTargetStates().observations, formulaInfo.getTargetStates().states, formulaInfo.getSinkStates().states, smtSolverFactory);
                        memlessSearch.findNewStrategyForSomeState(5);
                    } else if (pomdpSettings.getMemlessSearchMethod() == "iterative") {
                        storm::pomdp::MemlessStrategySearchQualitative<ValueType> memlessSearch(*pomdp, formulaInfo.getTargetStates().observations, formulaInfo.getTargetStates().states, formulaInfo.getSinkStates().states, smtSolverFactory);
                        memlessSearch.findNewStrategyForSomeState(5);
                    } else {
                        STORM_LOG_ERROR("This method is not implemented.");
                    }
                    analysisPerformed = true;
                }
                if (pomdpSettings.isCheckFullyObservableSet()) {
                    STORM_PRINT_AND_LOG("Analyzing the formula on the fully observable MDP ... ");
                    auto resultPtr = storm::api::verifyWithSparseEngine<ValueType>(pomdp->template as<storm::models::sparse::Mdp<ValueType>>(), storm::api::createTask<ValueType>(formula.asSharedPointer(), true));
                    if (resultPtr) {
                        auto result = resultPtr->template asExplicitQuantitativeCheckResult<ValueType>();
                        result.filter(storm::modelchecker::ExplicitQualitativeCheckResult(pomdp->getInitialStates()));
                        if (storm::utility::resources::isTerminate()) {
                            STORM_PRINT_AND_LOG("\nResult till abort: ")
                        } else {
                            STORM_PRINT_AND_LOG("\nResult: ")
                        }
                        printResult(result.getMin(), result.getMax());
                        STORM_PRINT_AND_LOG(std::endl);
                    } else {
                        STORM_PRINT_AND_LOG("\nResult: Not available." << std::endl);
                    }
                    analysisPerformed = true;
                }
                return analysisPerformed;
            }
            
            
            template<typename ValueType, storm::dd::DdType DdType>
            bool performTransformation(std::shared_ptr<storm::models::sparse::Pomdp<ValueType>>& pomdp, storm::logic::Formula const& formula) {
                auto const& pomdpSettings = storm::settings::getModule<storm::settings::modules::POMDPSettings>();
                bool transformationPerformed = false;
                bool memoryUnfolded = false;
                if (pomdpSettings.getMemoryBound() > 1) {
                    STORM_PRINT_AND_LOG("Computing the unfolding for memory bound " << pomdpSettings.getMemoryBound() << " and memory pattern '" << storm::storage::toString(pomdpSettings.getMemoryPattern()) << "' ...");
                    storm::storage::PomdpMemory memory = storm::storage::PomdpMemoryBuilder().build(pomdpSettings.getMemoryPattern(), pomdpSettings.getMemoryBound());
                    std::cout << memory.toString() << std::endl;
                    storm::transformer::PomdpMemoryUnfolder<ValueType> memoryUnfolder(*pomdp, memory);
                    pomdp = memoryUnfolder.transform();
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    pomdp->printModelInformationToStream(std::cout);
                    transformationPerformed = true;
                    memoryUnfolded = true;
                }
        
                // From now on the pomdp is considered memoryless
        
                if (pomdpSettings.isMecReductionSet()) {
                    STORM_PRINT_AND_LOG("Eliminating mec choices ...");
                    // Note: Elimination of mec choices only preserves memoryless schedulers.
                    uint64_t oldChoiceCount = pomdp->getNumberOfChoices();
                    storm::transformer::GlobalPomdpMecChoiceEliminator<ValueType> mecChoiceEliminator(*pomdp);
                    pomdp = mecChoiceEliminator.transform(formula);
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    STORM_PRINT_AND_LOG(oldChoiceCount - pomdp->getNumberOfChoices() << " choices eliminated through MEC choice elimination." << std::endl);
                    pomdp->printModelInformationToStream(std::cout);
                    transformationPerformed = true;
                }
        
                if (pomdpSettings.isTransformBinarySet() || pomdpSettings.isTransformSimpleSet()) {
                    if (pomdpSettings.isTransformSimpleSet()) {
                        STORM_PRINT_AND_LOG("Transforming the POMDP to a simple POMDP.");
                        pomdp = storm::transformer::BinaryPomdpTransformer<ValueType>().transform(*pomdp, true);
                    } else {
                        STORM_PRINT_AND_LOG("Transforming the POMDP to a binary POMDP.");
                        pomdp = storm::transformer::BinaryPomdpTransformer<ValueType>().transform(*pomdp, false);
                    }
                    pomdp->printModelInformationToStream(std::cout);
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    transformationPerformed = true;
                }
        
                if (pomdpSettings.isExportToParametricSet()) {
                    STORM_PRINT_AND_LOG("Transforming memoryless POMDP to pMC...");
                    storm::transformer::ApplyFiniteSchedulerToPomdp<ValueType> toPMCTransformer(*pomdp);
                    std::string transformMode = pomdpSettings.getFscApplicationTypeString();
                    auto pmc = toPMCTransformer.transform(storm::transformer::parsePomdpFscApplicationMode(transformMode));
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    pmc->printModelInformationToStream(std::cout);
                    STORM_PRINT_AND_LOG("Simplifying pMC...");
                    //if (generalSettings.isBisimulationSet()) {
                    pmc = storm::api::performBisimulationMinimization<storm::RationalFunction>(pmc->template as<storm::models::sparse::Dtmc<storm::RationalFunction>>(),{formula.asSharedPointer()}, storm::storage::BisimulationType::Strong)->template as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
        
                    //}
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    pmc->printModelInformationToStream(std::cout);
                    STORM_PRINT_AND_LOG("Exporting pMC...");
                    storm::analysis::ConstraintCollector<storm::RationalFunction> constraints(*pmc);
                    auto const& parameterSet = constraints.getVariables();
                    std::vector<storm::RationalFunctionVariable> parameters(parameterSet.begin(), parameterSet.end());
                    std::vector<std::string> parameterNames;
                    for (auto const& parameter : parameters) {
                        parameterNames.push_back(parameter.name());
                    }
                    storm::api::exportSparseModelAsDrn(pmc, pomdpSettings.getExportToParametricFilename(), parameterNames);
                    STORM_PRINT_AND_LOG(" done." << std::endl);
                    transformationPerformed = true;
                }
                if (transformationPerformed && !memoryUnfolded) {
                    STORM_PRINT_AND_LOG("Implicitly assumed restriction to memoryless schedulers for at least one transformation." << std::endl);
                }
                return transformationPerformed;
            }
            
            template<typename ValueType, storm::dd::DdType DdType>
            void processOptionsWithValueTypeAndDdLib(storm::cli::SymbolicInput const& symbolicInput, storm::cli::ModelProcessingInformation const& mpi) {
                auto const& pomdpSettings = storm::settings::getModule<storm::settings::modules::POMDPSettings>();
                
                auto model = storm::cli::buildPreprocessExportModelWithValueTypeAndDdlib<DdType, ValueType>(symbolicInput, mpi);
                if (!model) {
                    STORM_PRINT_AND_LOG("No input model given." << std::endl);
                    return;
                }
                STORM_LOG_THROW(model->getType() == storm::models::ModelType::Pomdp && model->isSparseModel(), storm::exceptions::WrongFormatException, "Expected a POMDP in sparse representation.");
            
                std::shared_ptr<storm::models::sparse::Pomdp<ValueType>> pomdp = model->template as<storm::models::sparse::Pomdp<ValueType>>();
                if (!pomdpSettings.isNoCanonicSet()) {
                    storm::transformer::MakePOMDPCanonic<ValueType> makeCanonic(*pomdp);
                    pomdp = makeCanonic.transform();
                }
                
                std::shared_ptr<storm::logic::Formula const> formula;
                if (!symbolicInput.properties.empty()) {
                    formula = symbolicInput.properties.front().getRawFormula();
                    STORM_PRINT_AND_LOG("Analyzing property '" << *formula << "'" << std::endl);
                    STORM_LOG_WARN_COND(symbolicInput.properties.size() == 1, "There is currently no support for multiple properties. All other properties will be ignored.");
                }
            
                if (pomdpSettings.isAnalyzeUniqueObservationsSet()) {
                    STORM_PRINT_AND_LOG("Analyzing states with unique observation ..." << std::endl);
                    storm::analysis::UniqueObservationStates<ValueType> uniqueAnalysis(*pomdp);
                    std::cout << uniqueAnalysis.analyse() << std::endl;
                }
            
                if (formula) {
                    auto formulaInfo = storm::pomdp::analysis::getFormulaInformation(*pomdp, *formula);
                    STORM_LOG_THROW(!formulaInfo.isUnsupported(), storm::exceptions::InvalidPropertyException, "The formula '" << *formula << "' is not supported by storm-pomdp.");
                    
                    storm::utility::Stopwatch sw(true);
                    // Note that formulaInfo contains state-based information which potentially needs to be updated during preprocessing
                    if (performPreprocessing<ValueType, DdType>(pomdp, formulaInfo, *formula)) {
                        sw.stop();
                        STORM_PRINT_AND_LOG("Time for graph-based POMDP (pre-)processing: " << sw << "s." << std::endl);
                        pomdp->printModelInformationToStream(std::cout);
                    }
                    
                    sw.restart();
                    if (performAnalysis<ValueType, DdType>(pomdp, formulaInfo, *formula)) {
                        sw.stop();
                        STORM_PRINT_AND_LOG("Time for POMDP analysis: " << sw << "s." << std::endl);
                    }
                    
                    sw.restart();
                    if (performTransformation<ValueType, DdType>(pomdp, *formula)) {
                        sw.stop();
                        STORM_PRINT_AND_LOG("Time for POMDP transformation(s): " << sw << "s." << std::endl);
                    }
                } else {
                    STORM_LOG_WARN("Nothing to be done. Did you forget to specify a formula?");
                }
            
            }
            
            template <storm::dd::DdType DdType>
            void processOptionsWithDdLib(storm::cli::SymbolicInput const& symbolicInput, storm::cli::ModelProcessingInformation const& mpi) {
                STORM_LOG_ERROR_COND(mpi.buildValueType == mpi.verificationValueType, "Build value type differs from verification value type. Will ignore Verification value type.");
                switch (mpi.buildValueType) {
                    case storm::cli::ModelProcessingInformation::ValueType::FinitePrecision:
                        processOptionsWithValueTypeAndDdLib<double, DdType>(symbolicInput, mpi);
                        break;
                    case storm::cli::ModelProcessingInformation::ValueType::Exact:
                        STORM_LOG_THROW(DdType == storm::dd::DdType::Sylvan, storm::exceptions::UnexpectedException, "Exact arithmetic is only supported with Dd library Sylvan.");
                        processOptionsWithValueTypeAndDdLib<storm::RationalNumber, storm::dd::DdType::Sylvan>(symbolicInput, mpi);
                        break;
                    default:
                        STORM_LOG_THROW(false, storm::exceptions::UnexpectedException, "Unexpected ValueType for model building.");
                }
            }
            
            void processOptions() {
                auto symbolicInput = storm::cli::parseSymbolicInput();
                storm::cli::ModelProcessingInformation mpi;
                std::tie(symbolicInput, mpi) = storm::cli::preprocessSymbolicInput(symbolicInput);
                switch (mpi.ddType) {
                    case storm::dd::DdType::CUDD:
                        processOptionsWithDdLib<storm::dd::DdType::CUDD>(symbolicInput, mpi);
                        break;
                    case storm::dd::DdType::Sylvan:
                        processOptionsWithDdLib<storm::dd::DdType::Sylvan>(symbolicInput, mpi);
                        break;
                    default:
                        STORM_LOG_THROW(false, storm::exceptions::UnexpectedException, "Unexpected Dd Type.");
                }
            }
        }
    }
}

/*!
 * Entry point for the pomdp backend.
 *
 * @param argc The argc argument of main().
 * @param argv The argv argument of main().
 * @return Return code, 0 if successfull, not 0 otherwise.
 */
int main(const int argc, const char** argv) {
    //try {
        storm::utility::setUp();
        storm::cli::printHeader("Storm-pomdp", argc, argv);
        storm::settings::initializePomdpSettings("Storm-POMDP", "storm-pomdp");

        bool optionsCorrect = storm::cli::parseOptions(argc, argv);
        if (!optionsCorrect) {
            return -1;
        }
        storm::cli::setUrgentOptions();

        // Invoke storm-pomdp with obtained settings
        storm::pomdp::cli::processOptions();

        // All operations have now been performed, so we clean up everything and terminate.
        storm::utility::cleanUp();
        return 0;
    // } catch (storm::exceptions::BaseException const &exception) {
    //    STORM_LOG_ERROR("An exception caused Storm-pomdp to terminate. The message of the exception is: " << exception.what());
    //    return 1;
    //} catch (std::exception const &exception) {
    //    STORM_LOG_ERROR("An unexpected exception occurred and caused Storm-pomdp to terminate. The message of this exception is: " << exception.what());
    //    return 2;
    //}
}
