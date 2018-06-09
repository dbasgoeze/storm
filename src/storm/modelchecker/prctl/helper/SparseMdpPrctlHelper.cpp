#include "storm/modelchecker/prctl/helper/SparseMdpPrctlHelper.h"

#include <boost/container/flat_map.hpp>

#include "storm/modelchecker/results/ExplicitQuantitativeCheckResult.h"
#include "storm/modelchecker/hints/ExplicitModelCheckerHint.h"
#include "storm/modelchecker/prctl/helper/DsMpiUpperRewardBoundsComputer.h"
#include "storm/modelchecker/prctl/helper/BaierUpperRewardBoundsComputer.h"
#include "storm/modelchecker/prctl/helper/SparseMdpEndComponentInformation.h"

#include "storm/models/sparse/StandardRewardModel.h"

#include "storm/storage/MaximalEndComponentDecomposition.h"

#include "storm/utility/macros.h"
#include "storm/utility/vector.h"
#include "storm/utility/graph.h"

#include "storm/storage/expressions/Variable.h"
#include "storm/storage/expressions/Expression.h"
#include "storm/storage/Scheduler.h"

#include "storm/solver/MinMaxLinearEquationSolver.h"
#include "storm/solver/Multiplier.h"
#include "storm/solver/LpSolver.h"

#include "storm/settings/SettingsManager.h"
#include "storm/settings/modules/ModelCheckerSettings.h"
#include "storm/settings/modules/MinMaxEquationSolverSettings.h"
#include "storm/settings/modules/GeneralSettings.h"
#include "storm/settings/modules/CoreSettings.h"
#include "storm/settings/modules/IOSettings.h"

#include "storm/utility/Stopwatch.h"
#include "storm/utility/ProgressMeasurement.h"
#include "storm/utility/export.h"

#include "storm/environment/solver/MinMaxSolverEnvironment.h"

#include "storm/exceptions/InvalidStateException.h"
#include "storm/exceptions/InvalidPropertyException.h"
#include "storm/exceptions/InvalidSettingsException.h"
#include "storm/exceptions/IllegalFunctionCallException.h"
#include "storm/exceptions/IllegalArgumentException.h"
#include "storm/exceptions/UncheckedRequirementException.h"
#include "storm/exceptions/NotSupportedException.h"

namespace storm {
    namespace modelchecker {
        namespace helper {

            template<typename ValueType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeStepBoundedUntilProbabilities(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& psiStates, uint_fast64_t stepBound, ModelCheckerHint const& hint) {
                std::vector<ValueType> result(transitionMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                
                // Determine the states that have 0 probability of reaching the target states.
                storm::storage::BitVector maybeStates;
                if (hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().getComputeOnlyMaybeStates()) {
                    maybeStates = hint.template asExplicitModelCheckerHint<ValueType>().getMaybeStates();
                } else {
                    if (goal.minimize()) {
                        maybeStates = storm::utility::graph::performProbGreater0A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, phiStates, psiStates, true, stepBound);
                    } else {
                        maybeStates = storm::utility::graph::performProbGreater0E(backwardTransitions, phiStates, psiStates, true, stepBound);
                    }
                    maybeStates &= ~psiStates;
                }
                
                STORM_LOG_INFO("Preprocessing: " << maybeStates.getNumberOfSetBits() << " non-target states with probability greater 0.");
                
                if (!maybeStates.empty()) {
                    // We can eliminate the rows and columns from the original transition probability matrix that have probability 0.
                    storm::storage::SparseMatrix<ValueType> submatrix = transitionMatrix.getSubmatrix(true, maybeStates, maybeStates, false);
                    std::vector<ValueType> b = transitionMatrix.getConstrainedRowGroupSumVector(maybeStates, psiStates);
                    
                    // Create the vector with which to multiply.
                    std::vector<ValueType> subresult(maybeStates.getNumberOfSetBits());
                    
                    auto multiplier = storm::solver::MultiplierFactory<ValueType>().create(env, submatrix);
                    multiplier->repeatedMultiplyAndReduce(env, goal.direction(), subresult, &b, stepBound);
                    
                    // Set the values of the resulting vector accordingly.
                    storm::utility::vector::setVectorValues(result, maybeStates, subresult);
                }
                storm::utility::vector::setVectorValues(result, psiStates, storm::utility::one<ValueType>());
                
                return result;
            }

            template<typename ValueType>
            std::vector<ValueType> analyzeTrivialMdpEpochModel(OptimizationDirection dir, typename rewardbounded::MultiDimensionalRewardUnfolding<ValueType, true>::EpochModel& epochModel) {
                // Assert that the epoch model is indeed trivial
                assert(epochModel.epochMatrix.getEntryCount() == 0);
                
                std::vector<ValueType> epochResult;
                epochResult.reserve(epochModel.epochInStates.getNumberOfSetBits());
                
                auto stepSolutionIt = epochModel.stepSolutions.begin();
                auto stepChoiceIt = epochModel.stepChoices.begin();
                for (auto const& state : epochModel.epochInStates) {
                    // Obtain the best choice for this state
                    ValueType bestValue;
                    uint64_t lastChoice = epochModel.epochMatrix.getRowGroupIndices()[state + 1];
                    bool isFirstChoice = true;
                    for (uint64_t choice = epochModel.epochMatrix.getRowGroupIndices()[state]; choice < lastChoice; ++choice) {
                        while (*stepChoiceIt < choice) {
                            ++stepChoiceIt;
                            ++stepSolutionIt;
                        }
                        
                        ValueType choiceValue = storm::utility::zero<ValueType>();
                        if (epochModel.objectiveRewardFilter.front().get(choice)) {
                            choiceValue += epochModel.objectiveRewards.front()[choice];
                        }
                        if (*stepChoiceIt == choice) {
                            choiceValue += *stepSolutionIt;
                        }
                        
                        if (isFirstChoice) {
                            bestValue = std::move(choiceValue);
                            isFirstChoice = false;
                        } else {
                            if (storm::solver::minimize(dir)) {
                                if (choiceValue < bestValue) {
                                    bestValue = std::move(choiceValue);
                                }
                            } else {
                                if (choiceValue > bestValue) {
                                    bestValue = std::move(choiceValue);
                                }
                            }
                        }
                    }
                    // Insert the solution w.r.t. this choice
                    epochResult.push_back(std::move(bestValue));
                }
                return epochResult;
            }
            
            template<typename ValueType>
            std::vector<ValueType> analyzeNonTrivialMdpEpochModel(Environment const& env, OptimizationDirection dir, typename rewardbounded::MultiDimensionalRewardUnfolding<ValueType, true>::EpochModel& epochModel, std::vector<ValueType>& x, std::vector<ValueType>& b, std::unique_ptr<storm::solver::MinMaxLinearEquationSolver<ValueType>>& minMaxSolver, boost::optional<ValueType> const& lowerBound, boost::optional<ValueType> const& upperBound) {
 
                // Update some data for the case that the Matrix has changed
                if (epochModel.epochMatrixChanged) {
                    x.assign(epochModel.epochMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                    storm::solver::GeneralMinMaxLinearEquationSolverFactory<ValueType> minMaxLinearEquationSolverFactory;
                    minMaxSolver = minMaxLinearEquationSolverFactory.create(env, epochModel.epochMatrix);
                    minMaxSolver->setHasUniqueSolution();
                    minMaxSolver->setOptimizationDirection(dir);
                    minMaxSolver->setCachingEnabled(true);
                    minMaxSolver->setTrackScheduler(true);
                    auto req = minMaxSolver->getRequirements(env, dir, false);
                    if (lowerBound) {
                        minMaxSolver->setLowerBound(lowerBound.get());
                        req.clearLowerBounds();
                    }
                    if (upperBound) {
                        minMaxSolver->setUpperBound(upperBound.get());
                        req.clearUpperBounds();
                    }
                    STORM_LOG_THROW(!req.hasEnabledCriticalRequirement(), storm::exceptions::UncheckedRequirementException, "Solver requirements " + req.getEnabledRequirementsAsString() + " not checked.");
                    minMaxSolver->setRequirementsChecked();
                } else {
                    auto choicesTmp = minMaxSolver->getSchedulerChoices();
                    minMaxSolver->setInitialScheduler(std::move(choicesTmp));
                }
                
                // Prepare the right hand side of the equation system
                b.assign(epochModel.epochMatrix.getRowCount(), storm::utility::zero<ValueType>());
                std::vector<ValueType> const& objectiveValues = epochModel.objectiveRewards.front();
                for (auto const& choice : epochModel.objectiveRewardFilter.front()) {
                    b[choice] = objectiveValues[choice];
                }
                auto stepSolutionIt = epochModel.stepSolutions.begin();
                for (auto const& choice : epochModel.stepChoices) {
                    b[choice] += *stepSolutionIt;
                    ++stepSolutionIt;
                }
                assert(stepSolutionIt == epochModel.stepSolutions.end());
                
                // Solve the minMax equation system
                minMaxSolver->solveEquations(env, x, b);
                
                return storm::utility::vector::filterVector(x, epochModel.epochInStates);
            }
            
            template<typename ValueType>
            std::map<storm::storage::sparse::state_type, ValueType> SparseMdpPrctlHelper<ValueType>::computeRewardBoundedValues(Environment const& env, OptimizationDirection dir, rewardbounded::MultiDimensionalRewardUnfolding<ValueType, true>& rewardUnfolding, storm::storage::BitVector const& initialStates) {
                storm::utility::Stopwatch swAll(true), swBuild, swCheck;
                
                // Get lower and upper bounds for the solution.
                auto lowerBound = rewardUnfolding.getLowerObjectiveBound();
                auto upperBound = rewardUnfolding.getUpperObjectiveBound();
                
                // Initialize epoch models
                auto initEpoch = rewardUnfolding.getStartEpoch();
                auto epochOrder = rewardUnfolding.getEpochComputationOrder(initEpoch);
                
                // initialize data that will be needed for each epoch
                std::vector<ValueType> x, b;
                std::unique_ptr<storm::solver::MinMaxLinearEquationSolver<ValueType>> minMaxSolver;

                ValueType precision = rewardUnfolding.getRequiredEpochModelPrecision(initEpoch, storm::utility::convertNumber<ValueType>(storm::settings::getModule<storm::settings::modules::GeneralSettings>().getPrecision()));
                Environment preciseEnv = env;
                preciseEnv.solver().minMax().setPrecision(storm::utility::convertNumber<storm::RationalNumber>(precision));
                
                // In case of cdf export we store the necessary data.
                std::vector<std::vector<ValueType>> cdfData;

                storm::utility::ProgressMeasurement progress("epochs");
                progress.setMaxCount(epochOrder.size());
                progress.startNewMeasurement(0);
                uint64_t numCheckedEpochs = 0;
                for (auto const& epoch : epochOrder) {
                    swBuild.start();
                    auto& epochModel = rewardUnfolding.setCurrentEpoch(epoch);
                    swBuild.stop(); swCheck.start();
                    // If the epoch matrix is empty we do not need to solve a linear equation system
                    if (epochModel.epochMatrix.getEntryCount() == 0) {
                        rewardUnfolding.setSolutionForCurrentEpoch(analyzeTrivialMdpEpochModel<ValueType>(dir, epochModel));
                    } else {
                        rewardUnfolding.setSolutionForCurrentEpoch(analyzeNonTrivialMdpEpochModel<ValueType>(preciseEnv, dir, epochModel, x, b, minMaxSolver, lowerBound, upperBound));
                    }
                    swCheck.stop();
                    if (storm::settings::getModule<storm::settings::modules::IOSettings>().isExportCdfSet() && !rewardUnfolding.getEpochManager().hasBottomDimension(epoch)) {
                        std::vector<ValueType> cdfEntry;
                        for (uint64_t i = 0; i < rewardUnfolding.getEpochManager().getDimensionCount(); ++i) {
                            uint64_t offset = rewardUnfolding.getDimension(i).isUpperBounded ? 0 : 1;
                            cdfEntry.push_back(storm::utility::convertNumber<ValueType>(rewardUnfolding.getEpochManager().getDimensionOfEpoch(epoch, i) + offset) * rewardUnfolding.getDimension(i).scalingFactor);
                        }
                        cdfEntry.push_back(rewardUnfolding.getInitialStateResult(epoch));
                        cdfData.push_back(std::move(cdfEntry));
                    }
                    ++numCheckedEpochs;
                    progress.updateProgress(numCheckedEpochs);
                }
                
                std::map<storm::storage::sparse::state_type, ValueType> result;
                for (auto const& initState : initialStates) {
                    result[initState] = rewardUnfolding.getInitialStateResult(initEpoch, initState);
                }
                
                swAll.stop();
                
                if (storm::settings::getModule<storm::settings::modules::IOSettings>().isExportCdfSet()) {
                    std::vector<std::string> headers;
                    for (uint64_t i = 0; i < rewardUnfolding.getEpochManager().getDimensionCount(); ++i) {
                        headers.push_back(rewardUnfolding.getDimension(i).formula->toString());
                    }
                    headers.push_back("Result");
                    storm::utility::exportDataToCSVFile<ValueType, std::string, std::string>(storm::settings::getModule<storm::settings::modules::IOSettings>().getExportCdfDirectory() + "cdf.csv", cdfData, headers);
                }

                
                if (storm::settings::getModule<storm::settings::modules::CoreSettings>().isShowStatisticsSet()) {
                    STORM_PRINT_AND_LOG("---------------------------------" << std::endl);
                    STORM_PRINT_AND_LOG("Statistics:" << std::endl);
                    STORM_PRINT_AND_LOG("---------------------------------" << std::endl);
                    STORM_PRINT_AND_LOG("          #checked epochs: " << epochOrder.size() << "." << std::endl);
                    STORM_PRINT_AND_LOG("             overall Time: " << swAll << "." << std::endl);
                    STORM_PRINT_AND_LOG("Epoch Model building Time: " << swBuild << "." << std::endl);
                    STORM_PRINT_AND_LOG("Epoch Model checking Time: " << swCheck << "." << std::endl);
                    STORM_PRINT_AND_LOG("---------------------------------" << std::endl);
                }
                
                return  result;
            }

            template<typename ValueType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeNextProbabilities(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::BitVector const& nextStates) {

                // Create the vector with which to multiply and initialize it correctly.
                std::vector<ValueType> result(transitionMatrix.getRowGroupCount());
                storm::utility::vector::setVectorValues(result, nextStates, storm::utility::one<ValueType>());
                
                auto multiplier = storm::solver::MultiplierFactory<ValueType>().create(env, transitionMatrix);
                multiplier->multiplyAndReduce(env, dir, result, nullptr, result);
                
                return result;
            }
            
            template<typename ValueType>
            std::vector<uint_fast64_t> computeValidSchedulerHint(Environment const& env, SolutionType const& type, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& maybeStates, storm::storage::BitVector const& filterStates, storm::storage::BitVector const& targetStates) {
                storm::storage::Scheduler<ValueType> validScheduler(maybeStates.size());

                if (type == SolutionType::UntilProbabilities) {
                    storm::utility::graph::computeSchedulerProbGreater0E(transitionMatrix, backwardTransitions, filterStates, targetStates, validScheduler, boost::none);
                } else if (type == SolutionType::ExpectedRewards) {
                    storm::utility::graph::computeSchedulerProb1E(maybeStates | targetStates, transitionMatrix, backwardTransitions, filterStates, targetStates, validScheduler);
                } else {
                    STORM_LOG_ASSERT(false, "Unexpected equation system type.");
                }
                
                // Extract the relevant parts of the scheduler for the solver.
                std::vector<uint_fast64_t> schedulerHint(maybeStates.getNumberOfSetBits());
                auto maybeIt = maybeStates.begin();
                for (auto& choice : schedulerHint) {
                    choice = validScheduler.getChoice(*maybeIt).getDeterministicChoice();
                    ++maybeIt;
                }
                return schedulerHint;
            }
            
            template<typename ValueType>
            struct SparseMdpHintType {
                SparseMdpHintType() : eliminateEndComponents(false), computeUpperBounds(false), uniqueSolution(false) {
                    // Intentionally left empty.
                }
                
                bool hasSchedulerHint() const {
                    return static_cast<bool>(schedulerHint);
                }

                bool hasValueHint() const {
                    return static_cast<bool>(valueHint);
                }

                bool hasLowerResultBound() const {
                    return static_cast<bool>(lowerResultBound);
                }

                ValueType const& getLowerResultBound() const {
                    return lowerResultBound.get();
                }
                
                bool hasUpperResultBound() const {
                    return static_cast<bool>(upperResultBound);
                }
                
                bool hasUpperResultBounds() const {
                    return static_cast<bool>(upperResultBounds);
                }

                ValueType const& getUpperResultBound() const {
                    return upperResultBound.get();
                }

                std::vector<ValueType>& getUpperResultBounds() {
                    return upperResultBounds.get();
                }
                
                std::vector<ValueType> const& getUpperResultBounds() const {
                    return upperResultBounds.get();
                }
                
                std::vector<uint64_t>& getSchedulerHint() {
                    return schedulerHint.get();
                }
                
                std::vector<ValueType>& getValueHint() {
                    return valueHint.get();
                }
                
                bool getEliminateEndComponents() const {
                    return eliminateEndComponents;
                }

                bool getComputeUpperBounds() {
                    return computeUpperBounds;
                }

                bool hasUniqueSolution() const {
                    return uniqueSolution;
                }
                
                boost::optional<std::vector<uint64_t>> schedulerHint;
                boost::optional<std::vector<ValueType>> valueHint;
                boost::optional<ValueType> lowerResultBound;
                boost::optional<ValueType> upperResultBound;
                boost::optional<std::vector<ValueType>> upperResultBounds;
                bool eliminateEndComponents;
                bool computeUpperBounds;
                bool uniqueSolution;
            };
            
            template<typename ValueType>
            void extractValueAndSchedulerHint(SparseMdpHintType<ValueType>& hintStorage, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& maybeStates, boost::optional<storm::storage::BitVector> const& selectedChoices, ModelCheckerHint const& hint, bool skipECWithinMaybeStatesCheck) {
                
                // Deal with scheduler hint.
                if (hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().hasSchedulerHint()) {
                    if (hintStorage.hasSchedulerHint()) {
                        STORM_LOG_WARN("A scheduler hint was provided, but the solver requires a specific one. The provided scheduler hint will be ignored.");
                    } else {
                        auto const& schedulerHint = hint.template asExplicitModelCheckerHint<ValueType>().getSchedulerHint();
                        std::vector<uint64_t> hintChoices;

                        // The scheduler hint is only applicable if it induces no BSCC consisting of maybe states.
                        bool hintApplicable;
                        if (!skipECWithinMaybeStatesCheck) {
                            hintChoices.reserve(maybeStates.size());
                            for (uint_fast64_t state = 0; state < maybeStates.size(); ++state) {
                                hintChoices.push_back(schedulerHint.getChoice(state).getDeterministicChoice());
                            }
                            hintApplicable = storm::utility::graph::performProb1(transitionMatrix.transposeSelectedRowsFromRowGroups(hintChoices), maybeStates, ~maybeStates).full();
                        } else {
                            hintApplicable = true;
                        }
    
                        if (hintApplicable) {
                            // Compute the hint w.r.t. the given subsystem.
                            hintChoices.clear();
                            hintChoices.reserve(maybeStates.getNumberOfSetBits());
                            for (auto const& state : maybeStates) {
                                uint_fast64_t hintChoice = schedulerHint.getChoice(state).getDeterministicChoice();
                                if (selectedChoices) {
                                    uint_fast64_t firstChoice = transitionMatrix.getRowGroupIndices()[state];
                                    uint_fast64_t lastChoice = firstChoice + hintChoice;
                                    hintChoice = 0;
                                    for (uint_fast64_t choice = selectedChoices->getNextSetIndex(firstChoice); choice < lastChoice; choice = selectedChoices->getNextSetIndex(choice + 1)) {
                                        ++hintChoice;
                                    }
                                }
                                hintChoices.push_back(hintChoice);
                            }
                            hintStorage.schedulerHint = std::move(hintChoices);
                        }
                    }
                }
                
                // Deal with solution value hint. Only applicable if there are no End Components consisting of maybe states.
                if (hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().hasResultHint() && (skipECWithinMaybeStatesCheck || hintStorage.hasSchedulerHint() || storm::utility::graph::performProb1A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, maybeStates, ~maybeStates).full())) {
                    hintStorage.valueHint = storm::utility::vector::filterVector(hint.template asExplicitModelCheckerHint<ValueType>().getResultHint(), maybeStates);
                }
            }
            
            template<typename ValueType>
            SparseMdpHintType<ValueType> computeHints(Environment const& env, SolutionType const& type, ModelCheckerHint const& hint, storm::OptimizationDirection const& dir, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& maybeStates, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& targetStates, boost::optional<storm::storage::BitVector> const& selectedChoices = boost::none) {
                SparseMdpHintType<ValueType> result;

                // The solution to the min-max equation system is unique if we minimize until probabilities or
                // maximize reachability rewards or if the hint tells us that there are no end-compontnes.
                result.uniqueSolution = (dir == storm::solver::OptimizationDirection::Minimize && type == SolutionType::UntilProbabilities)
                                      || (dir == storm::solver::OptimizationDirection::Maximize && type == SolutionType::ExpectedRewards)
                                      || (hint.isExplicitModelCheckerHint() && hint.asExplicitModelCheckerHint<ValueType>().getNoEndComponentsInMaybeStates());
                
                // Check for requirements of the solver.
                bool hasSchedulerHint = hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().hasSchedulerHint();
                storm::solver::GeneralMinMaxLinearEquationSolverFactory<ValueType> minMaxLinearEquationSolverFactory;
                storm::solver::MinMaxLinearEquationSolverRequirements requirements = minMaxLinearEquationSolverFactory.getRequirements(env, result.uniqueSolution, dir, hasSchedulerHint);
                if (requirements.hasEnabledRequirement()) {
                    // If the solver still requires no end-components, we have to eliminate them later.
                    if (requirements.noEndComponents()) {
                        STORM_LOG_ASSERT(!result.hasUniqueSolution(), "The solver requires to eliminate the end components although the solution is already assumed to be unique.");
                        STORM_LOG_DEBUG("Scheduling EC elimination, because the solver requires it.");
                        result.eliminateEndComponents = true;
                        // If end components have been eliminated we can assume a unique solution.
                        result.uniqueSolution = true;
                        requirements.clearNoEndComponents();
                    }
                    
                    // If the solver requires an initial scheduler, compute one now.
                    if (requirements.validInitialScheduler()) {
                        STORM_LOG_DEBUG("Computing valid scheduler, because the solver requires it.");
                        result.schedulerHint = computeValidSchedulerHint(env, type, transitionMatrix, backwardTransitions, maybeStates, phiStates, targetStates);
                        requirements.clearValidInitialScheduler();
                    }
                    
                    // Finally, we have information on the bounds depending on the problem type.
                    if (type == SolutionType::UntilProbabilities) {
                        requirements.clearBounds();
                    } else if (type == SolutionType::ExpectedRewards) {
                        requirements.clearLowerBounds();
                    }
                    if (requirements.upperBounds()) {
                        result.computeUpperBounds = true;
                        requirements.clearUpperBounds();
                    }
                    STORM_LOG_THROW(!requirements.hasEnabledCriticalRequirement(), storm::exceptions::UncheckedRequirementException, "Solver requirements " + requirements.getEnabledRequirementsAsString() + " not checked.");
                } else {
                    STORM_LOG_DEBUG("Solver has no requirements.");
                }

                // Only if there is no end component decomposition that we will need to do later, we use value and scheduler
                // hints from the provided hint.
                if (!result.eliminateEndComponents) {
                    extractValueAndSchedulerHint(result, transitionMatrix, backwardTransitions, maybeStates, selectedChoices, hint, result.uniqueSolution);
                } else {
                    STORM_LOG_WARN_COND(hint.isEmpty(), "A non-empty hint was provided, but its information will be disregarded.");
                }

                // Only set bounds if we did not obtain them from the hint.
                if (!result.hasLowerResultBound()) {
                    result.lowerResultBound = storm::utility::zero<ValueType>();
                }
                if (!result.hasUpperResultBound() && type == SolutionType::UntilProbabilities) {
                    result.upperResultBound = storm::utility::one<ValueType>();
                }
                
                // If we received an upper bound, we can drop the requirement to compute one.
                if (result.hasUpperResultBound()) {
                    result.computeUpperBounds = false;
                }

                return result;
            }
            
            template<typename ValueType>
            struct MaybeStateResult {
                MaybeStateResult(std::vector<ValueType>&& values) : values(std::move(values)) {
                    // Intentionally left empty.
                }
                
                bool hasScheduler() const {
                    return static_cast<bool>(scheduler);
                }
                
                std::vector<uint64_t> const& getScheduler() const {
                    return scheduler.get();
                }
                
                std::vector<ValueType> const& getValues() const {
                    return values;
                }
                
                std::vector<ValueType> values;
                boost::optional<std::vector<uint64_t>> scheduler;
            };
            
            template<typename ValueType>
            MaybeStateResult<ValueType> computeValuesForMaybeStates(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType>&& submatrix, std::vector<ValueType> const& b, bool produceScheduler, SparseMdpHintType<ValueType>& hint) {
                
                // Initialize the solution vector.
                std::vector<ValueType> x = hint.hasValueHint() ? std::move(hint.getValueHint()) : std::vector<ValueType>(submatrix.getRowGroupCount(), hint.hasLowerResultBound() ? hint.getLowerResultBound() : storm::utility::zero<ValueType>());
                
                // Set up the solver.
                storm::solver::GeneralMinMaxLinearEquationSolverFactory<ValueType> minMaxLinearEquationSolverFactory;
                std::unique_ptr<storm::solver::MinMaxLinearEquationSolver<ValueType>> solver = storm::solver::configureMinMaxLinearEquationSolver(env, std::move(goal), minMaxLinearEquationSolverFactory, std::move(submatrix));
                solver->setRequirementsChecked();
                solver->setHasUniqueSolution(hint.hasUniqueSolution());
                if (hint.hasLowerResultBound()) {
                    solver->setLowerBound(hint.getLowerResultBound());
                }
                if (hint.hasUpperResultBound()) {
                    solver->setUpperBound(hint.getUpperResultBound());
                }
                if (hint.hasUpperResultBounds()) {
                    solver->setUpperBounds(std::move(hint.getUpperResultBounds()));
                }
                if (hint.hasSchedulerHint()) {
                    solver->setInitialScheduler(std::move(hint.getSchedulerHint()));
                }
                solver->setTrackScheduler(produceScheduler);
                
                // Solve the corresponding system of equations.
                solver->solveEquations(env, x, b);
                
#ifndef NDEBUG
                // As a sanity check, make sure our local upper bounds were in fact correct.
                if (solver->hasUpperBound(storm::solver::AbstractEquationSolver<ValueType>::BoundType::Local)) {
                    auto resultIt = x.begin();
                    for (auto const& entry : solver->getUpperBounds()) {
                        STORM_LOG_ASSERT(*resultIt <= entry + env.solver().minMax().getPrecision(), "Expecting result value for state " << std::distance(x.begin(), resultIt) << " to be <= " << entry << ", but got " << *resultIt << ".");
                        ++resultIt;
                    }
                }
#endif
                
                // Create result.
                MaybeStateResult<ValueType> result(std::move(x));

                // If requested, return the requested scheduler.
                if (produceScheduler) {
                    result.scheduler = std::move(solver->getSchedulerChoices());
                }
                return result;
            }
            
            struct QualitativeStateSetsUntilProbabilities {
                storm::storage::BitVector maybeStates;
                storm::storage::BitVector statesWithProbability0;
                storm::storage::BitVector statesWithProbability1;
            };
            
            template<typename ValueType>
            QualitativeStateSetsUntilProbabilities getQualitativeStateSetsUntilProbabilitiesFromHint(ModelCheckerHint const& hint) {
                QualitativeStateSetsUntilProbabilities result;
                result.maybeStates = hint.template asExplicitModelCheckerHint<ValueType>().getMaybeStates();
                
                // Treat the states with probability zero/one.
                std::vector<ValueType> const& resultsForNonMaybeStates = hint.template asExplicitModelCheckerHint<ValueType>().getResultHint();
                result.statesWithProbability1 = storm::storage::BitVector(result.maybeStates.size());
                result.statesWithProbability0 = storm::storage::BitVector(result.maybeStates.size());
                storm::storage::BitVector nonMaybeStates = ~result.maybeStates;
                for (auto const& state : nonMaybeStates) {
                    if (storm::utility::isOne(resultsForNonMaybeStates[state])) {
                        result.statesWithProbability1.set(state, true);
                    } else {
                        STORM_LOG_THROW(storm::utility::isZero(resultsForNonMaybeStates[state]), storm::exceptions::IllegalArgumentException, "Expected that the result hint specifies probabilities in {0,1} for non-maybe states");
                        result.statesWithProbability0.set(state, true);
                    }
                }
                
                return result;
            }
            
            template<typename ValueType>
            QualitativeStateSetsUntilProbabilities computeQualitativeStateSetsUntilProbabilities(storm::solver::SolveGoal<ValueType> const& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& psiStates) {
                QualitativeStateSetsUntilProbabilities result;

                // Get all states that have probability 0 and 1 of satisfying the until-formula.
                std::pair<storm::storage::BitVector, storm::storage::BitVector> statesWithProbability01;
                if (goal.minimize()) {
                    statesWithProbability01 = storm::utility::graph::performProb01Min(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, phiStates, psiStates);
                } else {
                    statesWithProbability01 = storm::utility::graph::performProb01Max(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, phiStates, psiStates);
                }
                result.statesWithProbability0 = std::move(statesWithProbability01.first);
                result.statesWithProbability1 = std::move(statesWithProbability01.second);
                result.maybeStates = ~(result.statesWithProbability0 | result.statesWithProbability1);
                
                return result;
            }
            
            template<typename ValueType>
            QualitativeStateSetsUntilProbabilities getQualitativeStateSetsUntilProbabilities(storm::solver::SolveGoal<ValueType> const& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& psiStates, ModelCheckerHint const& hint) {
                if (hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().getComputeOnlyMaybeStates()) {
                    return getQualitativeStateSetsUntilProbabilitiesFromHint<ValueType>(hint);
                } else {
                    return computeQualitativeStateSetsUntilProbabilities(goal, transitionMatrix, backwardTransitions, phiStates, psiStates);
                }
            }
            
            template<typename ValueType>
            void extractSchedulerChoices(storm::storage::Scheduler<ValueType>& scheduler, std::vector<uint_fast64_t> const& subChoices, storm::storage::BitVector const& maybeStates) {
                auto subChoiceIt = subChoices.begin();
                for (auto maybeState : maybeStates) {
                    scheduler.setChoice(*subChoiceIt, maybeState);
                    ++subChoiceIt;
                }
                assert(subChoiceIt == subChoices.end());
            }
            
            template<typename ValueType>
            void extendScheduler(storm::storage::Scheduler<ValueType>& scheduler, storm::solver::SolveGoal<ValueType> const& goal, QualitativeStateSetsUntilProbabilities const& qualitativeStateSets, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& psiStates) {
                
                // Finally, if we need to produce a scheduler, we also need to figure out the parts of the scheduler for
                // the states with probability 1 or 0 (depending on whether we maximize or minimize).
                // We also need to define some arbitrary choice for the remaining states to obtain a fully defined scheduler.
                if (goal.minimize()) {
                    storm::utility::graph::computeSchedulerProb0E(qualitativeStateSets.statesWithProbability0, transitionMatrix, scheduler);
                    for (auto const& prob1State : qualitativeStateSets.statesWithProbability1) {
                        scheduler.setChoice(0, prob1State);
                    }
                } else {
                    storm::utility::graph::computeSchedulerProb1E(qualitativeStateSets.statesWithProbability1, transitionMatrix, backwardTransitions, phiStates, psiStates, scheduler);
                    for (auto const& prob0State : qualitativeStateSets.statesWithProbability0) {
                        scheduler.setChoice(0, prob0State);
                    }
                }
            }
            
            template<typename ValueType>
            void computeFixedPointSystemUntilProbabilities(storm::solver::SolveGoal<ValueType>& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, QualitativeStateSetsUntilProbabilities const& qualitativeStateSets, storm::storage::SparseMatrix<ValueType>& submatrix, std::vector<ValueType>& b) {
                // First, we can eliminate the rows and columns from the original transition probability matrix for states
                // whose probabilities are already known.
                submatrix = transitionMatrix.getSubmatrix(true, qualitativeStateSets.maybeStates, qualitativeStateSets.maybeStates, false);
                
                // Prepare the right-hand side of the equation system. For entry i this corresponds to
                // the accumulated probability of going from state i to some state that has probability 1.
                b = transitionMatrix.getConstrainedRowGroupSumVector(qualitativeStateSets.maybeStates, qualitativeStateSets.statesWithProbability1);
                
                // If the solve goal has relevant values, we need to adjust them.
                goal.restrictRelevantValues(qualitativeStateSets.maybeStates);
            }
            
            template<typename ValueType>
            boost::optional<SparseMdpEndComponentInformation<ValueType>> computeFixedPointSystemUntilProbabilitiesEliminateEndComponents(storm::solver::SolveGoal<ValueType>& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, QualitativeStateSetsUntilProbabilities const& qualitativeStateSets, storm::storage::SparseMatrix<ValueType>& submatrix, std::vector<ValueType>& b) {
                
                // Get the set of states that (under some scheduler) can stay in the set of maybestates forever
                storm::storage::BitVector candidateStates = storm::utility::graph::performProb0E(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, qualitativeStateSets.maybeStates, ~qualitativeStateSets.maybeStates);
                
                bool doDecomposition = !candidateStates.empty();
                
                storm::storage::MaximalEndComponentDecomposition<ValueType> endComponentDecomposition;
                if (doDecomposition) {
                    // Compute the states that are in MECs.
                    endComponentDecomposition = storm::storage::MaximalEndComponentDecomposition<ValueType>(transitionMatrix, backwardTransitions, candidateStates);
                }
                
                // Only do more work if there are actually end-components.
                if (doDecomposition && !endComponentDecomposition.empty()) {
                    STORM_LOG_DEBUG("Eliminating " << endComponentDecomposition.size() << " EC(s).");
                    SparseMdpEndComponentInformation<ValueType> result = SparseMdpEndComponentInformation<ValueType>::eliminateEndComponents(endComponentDecomposition, transitionMatrix, qualitativeStateSets.maybeStates, &qualitativeStateSets.statesWithProbability1, nullptr, nullptr, submatrix, &b, nullptr);
                    
                    // If the solve goal has relevant values, we need to adjust them.
                    if (goal.hasRelevantValues()) {
                        storm::storage::BitVector newRelevantValues(submatrix.getRowGroupCount());
                        for (auto state : goal.relevantValues()) {
                            if (qualitativeStateSets.maybeStates.get(state)) {
                                newRelevantValues.set(result.getRowGroupAfterElimination(state));
                            }
                        }
                        if (!newRelevantValues.empty()) {
                            goal.setRelevantValues(std::move(newRelevantValues));
                        }
                    }
                    
                    return result;
                } else {
                    STORM_LOG_DEBUG("Not eliminating ECs as there are none.");
                    computeFixedPointSystemUntilProbabilities(goal, transitionMatrix, qualitativeStateSets, submatrix, b);
                    
                    return boost::none;
                }
            }
            
            template<typename ValueType>
            MDPSparseModelCheckingHelperReturnType<ValueType> SparseMdpPrctlHelper<ValueType>::computeUntilProbabilities(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& phiStates, storm::storage::BitVector const& psiStates, bool qualitative, bool produceScheduler, ModelCheckerHint const& hint) {
                STORM_LOG_THROW(!qualitative || !produceScheduler, storm::exceptions::InvalidSettingsException, "Cannot produce scheduler when performing qualitative model checking only.");
                
                // Prepare resulting vector.
                std::vector<ValueType> result(transitionMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                
                // We need to identify the maybe states (states which have a probability for satisfying the until formula
                // that is strictly between 0 and 1) and the states that satisfy the formula with probablity 1 and 0, respectively.
                QualitativeStateSetsUntilProbabilities qualitativeStateSets = getQualitativeStateSetsUntilProbabilities(goal, transitionMatrix, backwardTransitions, phiStates, psiStates, hint);
                
                STORM_LOG_INFO("Preprocessing: " << qualitativeStateSets.statesWithProbability1.getNumberOfSetBits() << " states with probability 1, " << qualitativeStateSets.statesWithProbability0.getNumberOfSetBits() << " with probability 0 (" << qualitativeStateSets.maybeStates.getNumberOfSetBits() << " states remaining).");
                
                // Set values of resulting vector that are known exactly.
                storm::utility::vector::setVectorValues<ValueType>(result, qualitativeStateSets.statesWithProbability1, storm::utility::one<ValueType>());
                
                // If requested, we will produce a scheduler.
                std::unique_ptr<storm::storage::Scheduler<ValueType>> scheduler;
                if (produceScheduler) {
                    scheduler = std::make_unique<storm::storage::Scheduler<ValueType>>(transitionMatrix.getRowGroupCount());
                }
                
                // Check whether we need to compute exact probabilities for some states.
                if (qualitative) {
                    // Set the values for all maybe-states to 0.5 to indicate that their probability values are neither 0 nor 1.
                    storm::utility::vector::setVectorValues<ValueType>(result, qualitativeStateSets.maybeStates, storm::utility::convertNumber<ValueType>(0.5));
                } else {
                    if (!qualitativeStateSets.maybeStates.empty()) {
                        // In this case we have have to compute the remaining probabilities.
                        
                        // Obtain proper hint information either from the provided hint or from requirements of the solver.
                        SparseMdpHintType<ValueType> hintInformation = computeHints(env, SolutionType::UntilProbabilities, hint, goal.direction(), transitionMatrix, backwardTransitions, qualitativeStateSets.maybeStates, phiStates, qualitativeStateSets.statesWithProbability1);
                        
                        // Declare the components of the equation system we will solve.
                        storm::storage::SparseMatrix<ValueType> submatrix;
                        std::vector<ValueType> b;
                        
                        // If the hint information tells us that we have to eliminate MECs, we do so now.
                        boost::optional<SparseMdpEndComponentInformation<ValueType>> ecInformation;
                        if (hintInformation.getEliminateEndComponents()) {
                            ecInformation = computeFixedPointSystemUntilProbabilitiesEliminateEndComponents(goal, transitionMatrix, backwardTransitions, qualitativeStateSets, submatrix, b);

                            // Make sure we are not supposed to produce a scheduler if we actually eliminate end components.
                            STORM_LOG_THROW(!ecInformation || !ecInformation.get().getEliminatedEndComponents() || !produceScheduler, storm::exceptions::NotSupportedException, "Producing schedulers is not supported if end-components need to be eliminated for the solver.");
                        } else {
                            // Otherwise, we compute the standard equations.
                            computeFixedPointSystemUntilProbabilities(goal, transitionMatrix, qualitativeStateSets, submatrix, b);
                        }
                        
                        // Now compute the results for the maybe states.
                        MaybeStateResult<ValueType> resultForMaybeStates = computeValuesForMaybeStates(env, std::move(goal), std::move(submatrix), b, produceScheduler, hintInformation);
                        
                        // If we eliminated end components, we need to extract the result differently.
                        if (ecInformation && ecInformation.get().getEliminatedEndComponents()) {
                            ecInformation.get().setValues(result, qualitativeStateSets.maybeStates, resultForMaybeStates.getValues());
                        } else {
                            // Set values of resulting vector according to result.
                            storm::utility::vector::setVectorValues<ValueType>(result, qualitativeStateSets.maybeStates, resultForMaybeStates.getValues());
                        }

                        if (produceScheduler) {
                            extractSchedulerChoices(*scheduler, resultForMaybeStates.getScheduler(), qualitativeStateSets.maybeStates);
                        }
                    }
                }

                // Extend scheduler with choices for the states in the qualitative state sets.
                if (produceScheduler) {
                    extendScheduler(*scheduler, goal, qualitativeStateSets, transitionMatrix, backwardTransitions, phiStates, psiStates);
                }
                
                // Sanity check for created scheduler.
                STORM_LOG_ASSERT((!produceScheduler && !scheduler) || (!scheduler->isPartialScheduler() && scheduler->isDeterministicScheduler() && scheduler->isMemorylessScheduler()), "Unexpected format of obtained scheduler.");
                
                // Return result.
                return MDPSparseModelCheckingHelperReturnType<ValueType>(std::move(result), std::move(scheduler));
            }

            template<typename ValueType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeGloballyProbabilities(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& psiStates, bool qualitative, bool useMecBasedTechnique) {
                if (useMecBasedTechnique) {
                    storm::storage::MaximalEndComponentDecomposition<ValueType> mecDecomposition(transitionMatrix, backwardTransitions, psiStates);
                    storm::storage::BitVector statesInPsiMecs(transitionMatrix.getRowGroupCount());
                    for (auto const& mec : mecDecomposition) {
                        for (auto const& stateActionsPair : mec) {
                            statesInPsiMecs.set(stateActionsPair.first, true);
                        }
                    }
                    
                    return std::move(computeUntilProbabilities(env, std::move(goal), transitionMatrix, backwardTransitions, psiStates, statesInPsiMecs, qualitative, false).values);
                } else {
                    goal.oneMinus();
                    std::vector<ValueType> result = computeUntilProbabilities(env, std::move(goal), transitionMatrix, backwardTransitions, storm::storage::BitVector(transitionMatrix.getRowGroupCount(), true), ~psiStates, qualitative, false).values;
                    for (auto& element : result) {
                        element = storm::utility::one<ValueType>() - element;
                    }
                    return std::move(result);
                }
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeInstantaneousRewards(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, RewardModelType const& rewardModel, uint_fast64_t stepCount) {

                // Only compute the result if the model has a state-based reward this->getModel().
                STORM_LOG_THROW(rewardModel.hasStateRewards(), storm::exceptions::InvalidPropertyException, "Missing reward model for formula. Skipping formula.");
                
                // Initialize result to state rewards of the this->getModel().
                std::vector<ValueType> result(rewardModel.getStateRewardVector());
                
                auto multiplier = storm::solver::MultiplierFactory<ValueType>().create(env, transitionMatrix);
                multiplier->repeatedMultiplyAndReduce(env, goal.direction(), result, nullptr, stepCount);

                return result;
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeCumulativeRewards(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, RewardModelType const& rewardModel, uint_fast64_t stepBound) {

                // Only compute the result if the model has at least one reward this->getModel().
                STORM_LOG_THROW(!rewardModel.empty(), storm::exceptions::InvalidPropertyException, "Missing reward model for formula. Skipping formula.");
                
                // Compute the reward vector to add in each step based on the available reward models.
                std::vector<ValueType> totalRewardVector = rewardModel.getTotalRewardVector(transitionMatrix);
                
                // Initialize result to the zero vector.
                std::vector<ValueType> result(transitionMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                
                auto multiplier = storm::solver::MultiplierFactory<ValueType>().create(env, transitionMatrix);
                multiplier->repeatedMultiplyAndReduce(env, goal.direction(), result, &totalRewardVector, stepBound);
                
                return result;
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            MDPSparseModelCheckingHelperReturnType<ValueType> SparseMdpPrctlHelper<ValueType>::computeReachabilityRewards(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, RewardModelType const& rewardModel, storm::storage::BitVector const& targetStates, bool qualitative, bool produceScheduler, ModelCheckerHint const& hint) {
                // Only compute the result if the model has at least one reward this->getModel().
                STORM_LOG_THROW(!rewardModel.empty(), storm::exceptions::InvalidPropertyException, "Missing reward model for formula. Skipping formula.");
                return computeReachabilityRewardsHelper(env, std::move(goal), transitionMatrix, backwardTransitions,
                                                        [&rewardModel] (uint_fast64_t rowCount, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::BitVector const& maybeStates) {
                                                            return rewardModel.getTotalRewardVector(rowCount, transitionMatrix, maybeStates);
                                                        },
                                                        targetStates, qualitative, produceScheduler,
                                                        [&] () {
                                                            return rewardModel.getStatesWithZeroReward(transitionMatrix);
                                                        },
                                                        [&] () {
                                                            return rewardModel.getChoicesWithZeroReward(transitionMatrix);
                                                        },
                                                        hint);
            }
            
#ifdef STORM_HAVE_CARL
            template<typename ValueType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeReachabilityRewards(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::models::sparse::StandardRewardModel<storm::Interval> const& intervalRewardModel, bool lowerBoundOfIntervals, storm::storage::BitVector const& targetStates, bool qualitative) {
                // Only compute the result if the reward model is not empty.
                STORM_LOG_THROW(!intervalRewardModel.empty(), storm::exceptions::InvalidPropertyException, "Missing reward model for formula. Skipping formula.");
                return computeReachabilityRewardsHelper(env, std::move(goal), transitionMatrix, backwardTransitions, \
                                                        [&] (uint_fast64_t rowCount, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::BitVector const& maybeStates) {
                                                            std::vector<ValueType> result;
                                                            result.reserve(rowCount);
                                                            std::vector<storm::Interval> subIntervalVector = intervalRewardModel.getTotalRewardVector(rowCount, transitionMatrix, maybeStates);
                                                            for (auto const& interval : subIntervalVector) {
                                                                result.push_back(lowerBoundOfIntervals ? interval.lower() : interval.upper());
                                                            }
                                                            return result;
                                                        },
                                                        targetStates, qualitative, false,
                                                        [&] () {
                                                            return intervalRewardModel.getStatesWithFilter(transitionMatrix, [&](storm::Interval const& i) {return storm::utility::isZero(lowerBoundOfIntervals ? i.lower() : i.upper());});
                                                        },
                                                        [&] () {
                                                            return intervalRewardModel.getChoicesWithFilter(transitionMatrix, [&](storm::Interval const& i) {return storm::utility::isZero(lowerBoundOfIntervals ? i.lower() : i.upper());});
                                                        }).values;
            }
            
            template<>
            std::vector<storm::RationalNumber> SparseMdpPrctlHelper<storm::RationalNumber>::computeReachabilityRewards(Environment const& env, storm::solver::SolveGoal<storm::RationalNumber>&&, storm::storage::SparseMatrix<storm::RationalNumber> const&, storm::storage::SparseMatrix<storm::RationalNumber> const&, storm::models::sparse::StandardRewardModel<storm::Interval> const&, bool, storm::storage::BitVector const&, bool) {
                STORM_LOG_THROW(false, storm::exceptions::IllegalFunctionCallException, "Computing reachability rewards is unsupported for this data type.");
            }
#endif
            
            struct QualitativeStateSetsReachabilityRewards {
                storm::storage::BitVector maybeStates;
                storm::storage::BitVector infinityStates;
                storm::storage::BitVector rewardZeroStates;
            };

            template<typename ValueType>
            QualitativeStateSetsReachabilityRewards getQualitativeStateSetsReachabilityRewardsFromHint(ModelCheckerHint const& hint, storm::storage::BitVector const& targetStates) {
                QualitativeStateSetsReachabilityRewards result;
                result.maybeStates = hint.template asExplicitModelCheckerHint<ValueType>().getMaybeStates();
                
                // Treat the states with reward zero/infinity.
                std::vector<ValueType> const& resultsForNonMaybeStates = hint.template asExplicitModelCheckerHint<ValueType>().getResultHint();
                result.infinityStates = storm::storage::BitVector(result.maybeStates.size());
                result.rewardZeroStates = storm::storage::BitVector(result.maybeStates.size());
                storm::storage::BitVector nonMaybeStates = ~result.maybeStates;
                for (auto const& state : nonMaybeStates) {
                    if (storm::utility::isZero(resultsForNonMaybeStates[state])) {
                        result.rewardZeroStates.set(state, true);
                    } else {
                        STORM_LOG_THROW(storm::utility::isInfinity(resultsForNonMaybeStates[state]), storm::exceptions::IllegalArgumentException, "Expected that the result hint specifies probabilities in {0,infinity} for non-maybe states");
                        result.infinityStates.set(state, true);
                    }
                }
                return result;
            }
            
            template<typename ValueType>
            QualitativeStateSetsReachabilityRewards computeQualitativeStateSetsReachabilityRewards(storm::solver::SolveGoal<ValueType> const& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& targetStates, std::function<storm::storage::BitVector()> const& zeroRewardStatesGetter, std::function<storm::storage::BitVector()> const& zeroRewardChoicesGetter) {
                QualitativeStateSetsReachabilityRewards result;
                storm::storage::BitVector trueStates(transitionMatrix.getRowGroupCount(), true);
                if (goal.minimize()) {
                    result.infinityStates = storm::utility::graph::performProb1E(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, trueStates, targetStates);
                } else {
                    result.infinityStates = storm::utility::graph::performProb1A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, trueStates, targetStates);
                }
                result.infinityStates.complement();
                
                if (storm::settings::getModule<storm::settings::modules::ModelCheckerSettings>().isFilterRewZeroSet()) {
                    if (goal.minimize()) {
                        result.rewardZeroStates = storm::utility::graph::performProb1E(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, trueStates, targetStates, zeroRewardChoicesGetter());
                    } else {
                        result.rewardZeroStates = storm::utility::graph::performProb1A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, zeroRewardStatesGetter(), targetStates);
                    }
                } else {
                    result.rewardZeroStates = targetStates;
                }
                result.maybeStates = ~(result.rewardZeroStates | result.infinityStates);
                return result;
            }
            
            template<typename ValueType>
            QualitativeStateSetsReachabilityRewards getQualitativeStateSetsReachabilityRewards(storm::solver::SolveGoal<ValueType> const& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& targetStates, ModelCheckerHint const& hint, std::function<storm::storage::BitVector()> const& zeroRewardStatesGetter, std::function<storm::storage::BitVector()> const& zeroRewardChoicesGetter) {
                if (hint.isExplicitModelCheckerHint() && hint.template asExplicitModelCheckerHint<ValueType>().getComputeOnlyMaybeStates()) {
                    return getQualitativeStateSetsReachabilityRewardsFromHint<ValueType>(hint, targetStates);
                } else {
                    return computeQualitativeStateSetsReachabilityRewards(goal, transitionMatrix, backwardTransitions, targetStates, zeroRewardStatesGetter, zeroRewardChoicesGetter);
                }
            }
            
            template<typename ValueType>
            void extendScheduler(storm::storage::Scheduler<ValueType>& scheduler, storm::solver::SolveGoal<ValueType> const& goal, QualitativeStateSetsReachabilityRewards const& qualitativeStateSets, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& targetStates, std::function<storm::storage::BitVector()> const& zeroRewardChoicesGetter) {
                // Finally, if we need to produce a scheduler, we also need to figure out the parts of the scheduler for
                // the states with reward zero/infinity.
                if (goal.minimize()) {
                    storm::utility::graph::computeSchedulerProb1E(qualitativeStateSets.rewardZeroStates, transitionMatrix, backwardTransitions, qualitativeStateSets.rewardZeroStates, targetStates, scheduler, zeroRewardChoicesGetter());
                    for (auto const& state : qualitativeStateSets.infinityStates) {
                        scheduler.setChoice(0, state);
                    }
                } else {
                    storm::utility::graph::computeSchedulerProb0E(qualitativeStateSets.infinityStates, transitionMatrix, scheduler);
                    for (auto const& state : qualitativeStateSets.rewardZeroStates) {
                        scheduler.setChoice(0, state);
                    }
                }
            }
            
            template<typename ValueType>
            void extractSchedulerChoices(storm::storage::Scheduler<ValueType>& scheduler, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, std::vector<uint_fast64_t> const& subChoices, storm::storage::BitVector const& maybeStates, boost::optional<storm::storage::BitVector> const& selectedChoices) {
                auto subChoiceIt = subChoices.begin();
                if (selectedChoices) {
                    for (auto maybeState : maybeStates) {
                        // find the rowindex that corresponds to the selected row of the submodel
                        uint_fast64_t firstRowIndex = transitionMatrix.getRowGroupIndices()[maybeState];
                        uint_fast64_t selectedRowIndex = selectedChoices->getNextSetIndex(firstRowIndex);
                        for (uint_fast64_t choice = 0; choice < *subChoiceIt; ++choice) {
                            selectedRowIndex = selectedChoices->getNextSetIndex(selectedRowIndex + 1);
                        }
                        scheduler.setChoice(selectedRowIndex - firstRowIndex, maybeState);
                        ++subChoiceIt;
                    }
                } else {
                    for (auto maybeState : maybeStates) {
                        scheduler.setChoice(*subChoiceIt, maybeState);
                        ++subChoiceIt;
                    }
                }
                assert(subChoiceIt == subChoices.end());
            }
            
            template<typename ValueType>
            void computeFixedPointSystemReachabilityRewards(storm::solver::SolveGoal<ValueType>& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, QualitativeStateSetsReachabilityRewards const& qualitativeStateSets, boost::optional<storm::storage::BitVector> const& selectedChoices, std::function<std::vector<ValueType>(uint_fast64_t, storm::storage::SparseMatrix<ValueType> const&, storm::storage::BitVector const&)> const& totalStateRewardVectorGetter, storm::storage::SparseMatrix<ValueType>& submatrix, std::vector<ValueType>& b, std::vector<ValueType>* oneStepTargetProbabilities = nullptr) {
                // Remove rows and columns from the original transition probability matrix for states whose reward values are already known.
                // If there are infinity states, we additionally have to remove choices of maybeState that lead to infinity.
                if (qualitativeStateSets.infinityStates.empty()) {
                    submatrix = transitionMatrix.getSubmatrix(true, qualitativeStateSets.maybeStates, qualitativeStateSets.maybeStates, false);
                    b = totalStateRewardVectorGetter(submatrix.getRowCount(), transitionMatrix, qualitativeStateSets.maybeStates);
                    if (oneStepTargetProbabilities) {
                        (*oneStepTargetProbabilities) = transitionMatrix.getConstrainedRowGroupSumVector(qualitativeStateSets.maybeStates, qualitativeStateSets.rewardZeroStates);
                    }
                } else {
                    submatrix = transitionMatrix.getSubmatrix(false, *selectedChoices, qualitativeStateSets.maybeStates, false);
                    b = totalStateRewardVectorGetter(transitionMatrix.getRowCount(), transitionMatrix, storm::storage::BitVector(transitionMatrix.getRowGroupCount(), true));
                    storm::utility::vector::filterVectorInPlace(b, *selectedChoices);
                    if (oneStepTargetProbabilities) {
                        (*oneStepTargetProbabilities) = transitionMatrix.getConstrainedRowSumVector(*selectedChoices, qualitativeStateSets.rewardZeroStates);
                    }
                }
                
                // If the solve goal has relevant values, we need to adjust them.
                goal.restrictRelevantValues(qualitativeStateSets.maybeStates);
            }
            
            template<typename ValueType>
            boost::optional<SparseMdpEndComponentInformation<ValueType>> computeFixedPointSystemReachabilityRewardsEliminateEndComponents(storm::solver::SolveGoal<ValueType>& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, QualitativeStateSetsReachabilityRewards const& qualitativeStateSets, boost::optional<storm::storage::BitVector> const& selectedChoices, std::function<std::vector<ValueType>(uint_fast64_t, storm::storage::SparseMatrix<ValueType> const&, storm::storage::BitVector const&)> const& totalStateRewardVectorGetter, storm::storage::SparseMatrix<ValueType>& submatrix, std::vector<ValueType>& b, boost::optional<std::vector<ValueType>>& oneStepTargetProbabilities) {
                
                // Start by computing the choices with reward 0, as we only want ECs within this fragment.
                storm::storage::BitVector zeroRewardChoices(transitionMatrix.getRowCount());

                // Get the rewards of all choices.
                std::vector<ValueType> rewardVector = totalStateRewardVectorGetter(transitionMatrix.getRowCount(), transitionMatrix, storm::storage::BitVector(transitionMatrix.getRowGroupCount(), true));
                
                uint64_t index = 0;
                for (auto const& e : rewardVector) {
                    if (storm::utility::isZero(e)) {
                        zeroRewardChoices.set(index);
                    }
                    ++index;
                }
                
                // Compute the states that have some zero reward choice.
                storm::storage::BitVector candidateStates(qualitativeStateSets.maybeStates);
                for (auto state : qualitativeStateSets.maybeStates) {
                    bool keepState = false;
                    
                    for (auto row = transitionMatrix.getRowGroupIndices()[state], rowEnd = transitionMatrix.getRowGroupIndices()[state + 1]; row < rowEnd; ++row) {
                        if (zeroRewardChoices.get(row)) {
                            keepState = true;
                            break;
                        }
                    }
                    
                    if (!keepState) {
                        candidateStates.set(state, false);
                    }
                }
                
                // Only keep the candidate states that (under some scheduler) can stay in the set of candidates forever
                candidateStates = storm::utility::graph::performProb0E(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, candidateStates, ~candidateStates);
                
                bool doDecomposition = !candidateStates.empty();
                
                storm::storage::MaximalEndComponentDecomposition<ValueType> endComponentDecomposition;
                if (doDecomposition) {
                    // Then compute the states that are in MECs with zero reward.
                    endComponentDecomposition = storm::storage::MaximalEndComponentDecomposition<ValueType>(transitionMatrix, backwardTransitions, candidateStates, zeroRewardChoices);
                }
                
                // Only do more work if there are actually end-components.
                if (doDecomposition && !endComponentDecomposition.empty()) {
                    STORM_LOG_DEBUG("Eliminating " << endComponentDecomposition.size() << " ECs.");
                    SparseMdpEndComponentInformation<ValueType> result = SparseMdpEndComponentInformation<ValueType>::eliminateEndComponents(endComponentDecomposition, transitionMatrix, qualitativeStateSets.maybeStates, oneStepTargetProbabilities ? &qualitativeStateSets.rewardZeroStates : nullptr, selectedChoices ? &selectedChoices.get() : nullptr, &rewardVector, submatrix, oneStepTargetProbabilities ? &oneStepTargetProbabilities.get() : nullptr, &b);
                    
                    // If the solve goal has relevant values, we need to adjust them.
                    if (goal.hasRelevantValues()) {
                        storm::storage::BitVector newRelevantValues(submatrix.getRowGroupCount());
                        for (auto state : goal.relevantValues()) {
                            if (qualitativeStateSets.maybeStates.get(state)) {
                                newRelevantValues.set(result.getRowGroupAfterElimination(state));
                            }
                        }
                        if (!newRelevantValues.empty()) {
                            goal.setRelevantValues(std::move(newRelevantValues));
                        }
                    }
                    
                    return result;
                } else {
                    STORM_LOG_DEBUG("Not eliminating ECs as there are none.");
                    computeFixedPointSystemReachabilityRewards(goal, transitionMatrix, qualitativeStateSets, selectedChoices, totalStateRewardVectorGetter, submatrix, b, oneStepTargetProbabilities ? &oneStepTargetProbabilities.get() : nullptr);
                    return boost::none;
                }
            }
            
            template<typename ValueType>
            void computeUpperRewardBounds(SparseMdpHintType<ValueType>& hintInformation, storm::OptimizationDirection const& direction, storm::storage::SparseMatrix<ValueType> const& submatrix, std::vector<ValueType> const& choiceRewards, std::vector<ValueType> const& oneStepTargetProbabilities) {
                
                // For the min-case, we use DS-MPI, for the max-case variant 2 of the Baier et al. paper (CAV'17).
                if (direction == storm::OptimizationDirection::Minimize) {
                    DsMpiMdpUpperRewardBoundsComputer<ValueType> dsmpi(submatrix, choiceRewards, oneStepTargetProbabilities);
                    hintInformation.upperResultBounds = dsmpi.computeUpperBounds();
                } else {
                    BaierUpperRewardBoundsComputer<ValueType> baier(submatrix, choiceRewards, oneStepTargetProbabilities);
                    hintInformation.upperResultBound = baier.computeUpperBound();
                }
            }
            
            template<typename ValueType>
            MDPSparseModelCheckingHelperReturnType<ValueType> SparseMdpPrctlHelper<ValueType>::computeReachabilityRewardsHelper(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, std::function<std::vector<ValueType>(uint_fast64_t, storm::storage::SparseMatrix<ValueType> const&, storm::storage::BitVector const&)> const& totalStateRewardVectorGetter, storm::storage::BitVector const& targetStates, bool qualitative, bool produceScheduler, std::function<storm::storage::BitVector()> const& zeroRewardStatesGetter, std::function<storm::storage::BitVector()> const& zeroRewardChoicesGetter, ModelCheckerHint const& hint) {
                
                // Prepare resulting vector.
                std::vector<ValueType> result(transitionMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                
                // Determine which states have a reward that is infinity or less than infinity.
                QualitativeStateSetsReachabilityRewards qualitativeStateSets = getQualitativeStateSetsReachabilityRewards(goal, transitionMatrix, backwardTransitions, targetStates, hint, zeroRewardStatesGetter, zeroRewardChoicesGetter);
                
                STORM_LOG_INFO("Preprocessing: " << qualitativeStateSets.infinityStates.getNumberOfSetBits() << " states with reward infinity, " << qualitativeStateSets.rewardZeroStates.getNumberOfSetBits() << " states with reward zero (" << qualitativeStateSets.maybeStates.getNumberOfSetBits() << " states remaining).");

                storm::utility::vector::setVectorValues(result, qualitativeStateSets.infinityStates, storm::utility::infinity<ValueType>());
                
                // If requested, we will produce a scheduler.
                std::unique_ptr<storm::storage::Scheduler<ValueType>> scheduler;
                if (produceScheduler) {
                    scheduler = std::make_unique<storm::storage::Scheduler<ValueType>>(transitionMatrix.getRowGroupCount());
                }
                
                // Check whether we need to compute exact rewards for some states.
                if (qualitative) {
                    STORM_LOG_INFO("The rewards for the initial states were determined in a preprocessing step. No exact rewards were computed.");
                    // Set the values for all maybe-states to 1 to indicate that their reward values
                    // are neither 0 nor infinity.
                    storm::utility::vector::setVectorValues<ValueType>(result, qualitativeStateSets.maybeStates, storm::utility::one<ValueType>());
                } else {
                    if (!qualitativeStateSets.maybeStates.empty()) {
                        // In this case we have to compute the reward values for the remaining states.

                        // Store the choices that lead to non-infinity values. If none, all choices im maybe states can be selected.
                        boost::optional<storm::storage::BitVector> selectedChoices;
                        if (!qualitativeStateSets.infinityStates.empty()) {
                            selectedChoices = transitionMatrix.getRowFilter(qualitativeStateSets.maybeStates, ~qualitativeStateSets.infinityStates);
                        }
                        
                        // Obtain proper hint information either from the provided hint or from requirements of the solver.
                        SparseMdpHintType<ValueType> hintInformation = computeHints(env, SolutionType::ExpectedRewards, hint, goal.direction(), transitionMatrix, backwardTransitions, qualitativeStateSets.maybeStates, ~qualitativeStateSets.rewardZeroStates, qualitativeStateSets.rewardZeroStates, selectedChoices);
                        
                        // Declare the components of the equation system we will solve.
                        storm::storage::SparseMatrix<ValueType> submatrix;
                        std::vector<ValueType> b;

                        // If we need to compute upper bounds on the reward values, we need the one step probabilities
                        // to a target state.
                        boost::optional<std::vector<ValueType>> oneStepTargetProbabilities;
                        if (hintInformation.getComputeUpperBounds()) {
                            oneStepTargetProbabilities = std::vector<ValueType>();
                        }
                        
                        // If the hint information tells us that we have to eliminate MECs, we do so now.
                        boost::optional<SparseMdpEndComponentInformation<ValueType>> ecInformation;
                        if (hintInformation.getEliminateEndComponents()) {
                            ecInformation = computeFixedPointSystemReachabilityRewardsEliminateEndComponents(goal, transitionMatrix, backwardTransitions, qualitativeStateSets, selectedChoices, totalStateRewardVectorGetter, submatrix, b, oneStepTargetProbabilities);
                            
                            // Make sure we are not supposed to produce a scheduler if we actually eliminate end components.
                            STORM_LOG_THROW(!ecInformation || !ecInformation.get().getEliminatedEndComponents() || !produceScheduler, storm::exceptions::NotSupportedException, "Producing schedulers is not supported if end-components need to be eliminated for the solver.");
                        } else {
                            // Otherwise, we compute the standard equations.
                            computeFixedPointSystemReachabilityRewards(goal, transitionMatrix, qualitativeStateSets, selectedChoices, totalStateRewardVectorGetter, submatrix, b, oneStepTargetProbabilities ? &oneStepTargetProbabilities.get() : nullptr);
                        }
                        
                        // If we need to compute upper bounds, do so now.
                        if (hintInformation.getComputeUpperBounds()) {
                            STORM_LOG_ASSERT(oneStepTargetProbabilities, "Expecting one step target probability vector to be available.");
                            computeUpperRewardBounds(hintInformation, goal.direction(), submatrix, b, oneStepTargetProbabilities.get());
                        }
                        
                        // Now compute the results for the maybe states.
                        MaybeStateResult<ValueType> resultForMaybeStates = computeValuesForMaybeStates(env, std::move(goal), std::move(submatrix), b, produceScheduler, hintInformation);

                        // If we eliminated end components, we need to extract the result differently.
                        if (ecInformation && ecInformation.get().getEliminatedEndComponents()) {
                            ecInformation.get().setValues(result, qualitativeStateSets.maybeStates, resultForMaybeStates.getValues());
                        } else {
                            // Set values of resulting vector according to result.
                            storm::utility::vector::setVectorValues<ValueType>(result, qualitativeStateSets.maybeStates, resultForMaybeStates.getValues());
                        }
                        
                        if (produceScheduler) {
                            extractSchedulerChoices(*scheduler, transitionMatrix, resultForMaybeStates.getScheduler(), qualitativeStateSets.maybeStates, selectedChoices);
                        }
                    }
                }
                
                // Extend scheduler with choices for the states in the qualitative state sets.
                if (produceScheduler) {
                    extendScheduler(*scheduler, goal, qualitativeStateSets, transitionMatrix, backwardTransitions, targetStates, zeroRewardChoicesGetter);
                }
                
                // Sanity check for created scheduler.
                STORM_LOG_ASSERT((!produceScheduler && !scheduler) || (!scheduler->isPartialScheduler() && scheduler->isDeterministicScheduler() && scheduler->isMemorylessScheduler()), "Unexpected format of obtained scheduler.");

                return MDPSparseModelCheckingHelperReturnType<ValueType>(std::move(result), std::move(scheduler));
            }
            
            template<typename ValueType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeLongRunAverageProbabilities(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& psiStates) {
                
                // If there are no goal states, we avoid the computation and directly return zero.
                if (psiStates.empty()) {
                    return std::vector<ValueType>(transitionMatrix.getRowGroupCount(), storm::utility::zero<ValueType>());
                }
                
                // Likewise, if all bits are set, we can avoid the computation and set.
                if (psiStates.full()) {
                    return std::vector<ValueType>(transitionMatrix.getRowGroupCount(), storm::utility::one<ValueType>());
                }
                
                // Reduce long run average probabilities to long run average rewards by
                // building a reward model assigning one reward to every psi state
                std::vector<ValueType> stateRewards(psiStates.size(), storm::utility::zero<ValueType>());
                storm::utility::vector::setVectorValues(stateRewards, psiStates, storm::utility::one<ValueType>());
                storm::models::sparse::StandardRewardModel<ValueType> rewardModel(std::move(stateRewards));
                return computeLongRunAverageRewards(env, std::move(goal), transitionMatrix, backwardTransitions, rewardModel);
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            std::vector<ValueType> SparseMdpPrctlHelper<ValueType>::computeLongRunAverageRewards(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, RewardModelType const& rewardModel) {
                
                uint64_t numberOfStates = transitionMatrix.getRowGroupCount();

                // Start by decomposing the MDP into its MECs.
                storm::storage::MaximalEndComponentDecomposition<ValueType> mecDecomposition(transitionMatrix, backwardTransitions);
                
                // Get some data members for convenience.
                std::vector<uint_fast64_t> const& nondeterministicChoiceIndices = transitionMatrix.getRowGroupIndices();
                ValueType zero = storm::utility::zero<ValueType>();
                
                //first calculate LRA for the Maximal End Components.
                storm::storage::BitVector statesInMecs(numberOfStates);
                std::vector<uint_fast64_t> stateToMecIndexMap(transitionMatrix.getColumnCount());
                std::vector<ValueType> lraValuesForEndComponents(mecDecomposition.size(), zero);
                
                for (uint_fast64_t currentMecIndex = 0; currentMecIndex < mecDecomposition.size(); ++currentMecIndex) {
                    storm::storage::MaximalEndComponent const& mec = mecDecomposition[currentMecIndex];
                    
                    lraValuesForEndComponents[currentMecIndex] = computeLraForMaximalEndComponent(env, goal.direction(), transitionMatrix, rewardModel, mec);
                    
                    // Gather information for later use.
                    for (auto const& stateChoicesPair : mec) {
                        statesInMecs.set(stateChoicesPair.first);
                        stateToMecIndexMap[stateChoicesPair.first] = currentMecIndex;
                    }
                }
                
                // For fast transition rewriting, we build some auxiliary data structures.
                storm::storage::BitVector statesNotContainedInAnyMec = ~statesInMecs;
                uint_fast64_t firstAuxiliaryStateIndex = statesNotContainedInAnyMec.getNumberOfSetBits();
                uint_fast64_t lastStateNotInMecs = 0;
                uint_fast64_t numberOfStatesNotInMecs = 0;
                std::vector<uint_fast64_t> statesNotInMecsBeforeIndex;
                statesNotInMecsBeforeIndex.reserve(numberOfStates);
                for (auto state : statesNotContainedInAnyMec) {
                    while (lastStateNotInMecs <= state) {
                        statesNotInMecsBeforeIndex.push_back(numberOfStatesNotInMecs);
                        ++lastStateNotInMecs;
                    }
                    ++numberOfStatesNotInMecs;
                }
                
                // Finally, we are ready to create the SSP matrix and right-hand side of the SSP.
                std::vector<ValueType> b;
                uint64_t numberOfSspStates = numberOfStatesNotInMecs + mecDecomposition.size();

                typename storm::storage::SparseMatrixBuilder<ValueType> sspMatrixBuilder(0, numberOfSspStates, 0, false, true, numberOfSspStates);
                
                // If the source state is not contained in any MEC, we copy its choices (and perform the necessary modifications).
                uint_fast64_t currentChoice = 0;
                for (auto state : statesNotContainedInAnyMec) {
                    sspMatrixBuilder.newRowGroup(currentChoice);
                    
                    for (uint_fast64_t choice = nondeterministicChoiceIndices[state]; choice < nondeterministicChoiceIndices[state + 1]; ++choice, ++currentChoice) {
                        std::vector<ValueType> auxiliaryStateToProbabilityMap(mecDecomposition.size());
                        b.push_back(storm::utility::zero<ValueType>());
                        
                        for (auto element : transitionMatrix.getRow(choice)) {
                            if (statesNotContainedInAnyMec.get(element.getColumn())) {
                                // If the target state is not contained in an MEC, we can copy over the entry.
                                sspMatrixBuilder.addNextValue(currentChoice, statesNotInMecsBeforeIndex[element.getColumn()], element.getValue());
                            } else {
                                // If the target state is contained in MEC i, we need to add the probability to the corresponding field in the vector
                                // so that we are able to write the cumulative probability to the MEC into the matrix.
                                auxiliaryStateToProbabilityMap[stateToMecIndexMap[element.getColumn()]] += element.getValue();
                            }
                        }
                        
                        // Now insert all (cumulative) probability values that target an MEC.
                        for (uint_fast64_t mecIndex = 0; mecIndex < auxiliaryStateToProbabilityMap.size(); ++mecIndex) {
                            if (!storm::utility::isZero(auxiliaryStateToProbabilityMap[mecIndex])) {
                                sspMatrixBuilder.addNextValue(currentChoice, firstAuxiliaryStateIndex + mecIndex, auxiliaryStateToProbabilityMap[mecIndex]);
                            }
                        }
                    }
                }
                
                // Now we are ready to construct the choices for the auxiliary states.
                for (uint_fast64_t mecIndex = 0; mecIndex < mecDecomposition.size(); ++mecIndex) {
                    storm::storage::MaximalEndComponent const& mec = mecDecomposition[mecIndex];
                    sspMatrixBuilder.newRowGroup(currentChoice);
                    
                    for (auto const& stateChoicesPair : mec) {
                        uint_fast64_t state = stateChoicesPair.first;
                        boost::container::flat_set<uint_fast64_t> const& choicesInMec = stateChoicesPair.second;
                        
                        for (uint_fast64_t choice = nondeterministicChoiceIndices[state]; choice < nondeterministicChoiceIndices[state + 1]; ++choice) {
                            // If the choice is not contained in the MEC itself, we have to add a similar distribution to the auxiliary state.
                            if (choicesInMec.find(choice) == choicesInMec.end()) {
                                std::vector<ValueType> auxiliaryStateToProbabilityMap(mecDecomposition.size());
                                b.push_back(storm::utility::zero<ValueType>());
                                
                                for (auto element : transitionMatrix.getRow(choice)) {
                                    if (statesNotContainedInAnyMec.get(element.getColumn())) {
                                        // If the target state is not contained in an MEC, we can copy over the entry.
                                        sspMatrixBuilder.addNextValue(currentChoice, statesNotInMecsBeforeIndex[element.getColumn()], element.getValue());
                                    } else {
                                        // If the target state is contained in MEC i, we need to add the probability to the corresponding field in the vector
                                        // so that we are able to write the cumulative probability to the MEC into the matrix.
                                        auxiliaryStateToProbabilityMap[stateToMecIndexMap[element.getColumn()]] += element.getValue();
                                    }
                                }
                                
                                // Now insert all (cumulative) probability values that target an MEC.
                                for (uint_fast64_t targetMecIndex = 0; targetMecIndex < auxiliaryStateToProbabilityMap.size(); ++targetMecIndex) {
                                    if (!storm::utility::isZero(auxiliaryStateToProbabilityMap[targetMecIndex])) {
                                        sspMatrixBuilder.addNextValue(currentChoice, firstAuxiliaryStateIndex + targetMecIndex, auxiliaryStateToProbabilityMap[targetMecIndex]);
                                    }
                                }
                                
                                ++currentChoice;
                            }
                        }
                    }
                    
                    // For each auxiliary state, there is the option to achieve the reward value of the LRA associated with the MEC.
                    ++currentChoice;
                    b.push_back(lraValuesForEndComponents[mecIndex]);
                }
                
                // Finalize the matrix and solve the corresponding system of equations.
                storm::storage::SparseMatrix<ValueType> sspMatrix = sspMatrixBuilder.build(currentChoice, numberOfSspStates, numberOfSspStates);
                
                // Check for requirements of the solver.
                storm::solver::GeneralMinMaxLinearEquationSolverFactory<ValueType> minMaxLinearEquationSolverFactory;
                storm::solver::MinMaxLinearEquationSolverRequirements requirements = minMaxLinearEquationSolverFactory.getRequirements(env, true, goal.direction());
                requirements.clearBounds();
                STORM_LOG_THROW(!requirements.hasEnabledCriticalRequirement(), storm::exceptions::UncheckedRequirementException, "Solver requirements " + requirements.getEnabledRequirementsAsString() + " not checked.");

                
                std::vector<ValueType> sspResult(numberOfSspStates);
                goal.restrictRelevantValues(statesNotContainedInAnyMec);
                std::unique_ptr<storm::solver::MinMaxLinearEquationSolver<ValueType>> solver = storm::solver::configureMinMaxLinearEquationSolver(env, std::move(goal), minMaxLinearEquationSolverFactory, sspMatrix);
                solver->setLowerBound(storm::utility::zero<ValueType>());
                solver->setUpperBound(*std::max_element(lraValuesForEndComponents.begin(), lraValuesForEndComponents.end()));
                solver->setHasUniqueSolution();
                solver->setRequirementsChecked();
                solver->solveEquations(env, sspResult, b);
                
                // Prepare result vector.
                std::vector<ValueType> result(numberOfStates, zero);
                
                // Set the values for states not contained in MECs.
                storm::utility::vector::setVectorValues(result, statesNotContainedInAnyMec, sspResult);
                
                // Set the values for all states in MECs.
                for (auto state : statesInMecs) {
                    result[state] = sspResult[firstAuxiliaryStateIndex + stateToMecIndexMap[state]];
                }
                
                return result;
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            ValueType SparseMdpPrctlHelper<ValueType>::computeLraForMaximalEndComponent(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, RewardModelType const& rewardModel, storm::storage::MaximalEndComponent const& mec) {
                
                // If the mec only consists of a single state, we compute the LRA value directly
                if (++mec.begin() == mec.end()) {
                    uint64_t state = mec.begin()->first;
                    auto choiceIt = mec.begin()->second.begin();
                    ValueType result = rewardModel.getTotalStateActionReward(state, *choiceIt, transitionMatrix);
                    for (++choiceIt; choiceIt != mec.begin()->second.end(); ++choiceIt) {
                        if (storm::solver::minimize(dir)) {
                            result = std::min(result, rewardModel.getTotalStateActionReward(state, *choiceIt, transitionMatrix));
                        } else {
                            result = std::max(result, rewardModel.getTotalStateActionReward(state, *choiceIt, transitionMatrix));
                        }
                    }
                    return result;
                }
                
                // Solve MEC with the method specified in the settings
                storm::solver::LraMethod method = storm::settings::getModule<storm::settings::modules::MinMaxEquationSolverSettings>().getLraMethod();
                if (method == storm::solver::LraMethod::LinearProgramming) {
                    return computeLraForMaximalEndComponentLP(env, dir, transitionMatrix, rewardModel, mec);
                } else if (method == storm::solver::LraMethod::ValueIteration) {
                    return computeLraForMaximalEndComponentVI(env, dir, transitionMatrix, rewardModel, mec);
                } else {
                    STORM_LOG_THROW(false, storm::exceptions::InvalidSettingsException, "Unsupported technique.");
                }
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            ValueType SparseMdpPrctlHelper<ValueType>::computeLraForMaximalEndComponentVI(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, RewardModelType const& rewardModel, storm::storage::MaximalEndComponent const& mec) {
                
                // Initialize data about the mec
                storm::storage::BitVector mecStates(transitionMatrix.getRowGroupCount(), false);
                storm::storage::BitVector mecChoices(transitionMatrix.getRowCount(), false);
                for (auto const& stateChoicesPair : mec) {
                    mecStates.set(stateChoicesPair.first);
                    for (auto const& choice : stateChoicesPair.second) {
                        mecChoices.set(choice);
                    }
                }
                
                boost::container::flat_map<uint64_t, uint64_t> toSubModelStateMapping;
                uint64_t currState = 0;
                toSubModelStateMapping.reserve(mecStates.getNumberOfSetBits());
                for (auto const& mecState : mecStates) {
                    toSubModelStateMapping.insert(std::pair<uint64_t, uint64_t>(mecState, currState));
                    ++currState;
                }
                
                // Get a transition matrix that only considers the states and choices within the MEC
                storm::storage::SparseMatrixBuilder<ValueType> mecTransitionBuilder(mecChoices.getNumberOfSetBits(), mecStates.getNumberOfSetBits(), 0, true, true, mecStates.getNumberOfSetBits());
                std::vector<ValueType> choiceRewards;
                choiceRewards.reserve(mecChoices.getNumberOfSetBits());
                uint64_t currRow = 0;
                ValueType selfLoopProb = storm::utility::convertNumber<ValueType>(0.1); // todo try other values
                ValueType scalingFactor = storm::utility::one<ValueType>() - selfLoopProb;
                for (auto const& mecState : mecStates) {
                    mecTransitionBuilder.newRowGroup(currRow);
                    uint64_t groupStart = transitionMatrix.getRowGroupIndices()[mecState];
                    uint64_t groupEnd = transitionMatrix.getRowGroupIndices()[mecState + 1];
                    for (uint64_t choice = mecChoices.getNextSetIndex(groupStart); choice < groupEnd; choice = mecChoices.getNextSetIndex(choice + 1)) {
                        bool insertedDiagElement = false;
                        for (auto const& entry : transitionMatrix.getRow(choice)) {
                            uint64_t column = toSubModelStateMapping[entry.getColumn()];
                            if (!insertedDiagElement && entry.getColumn() > mecState) {
                                mecTransitionBuilder.addNextValue(currRow, toSubModelStateMapping[mecState], selfLoopProb);
                                insertedDiagElement = true;
                            }
                            if (!insertedDiagElement && entry.getColumn() == mecState) {
                                mecTransitionBuilder.addNextValue(currRow, column, selfLoopProb + scalingFactor * entry.getValue());
                                insertedDiagElement = true;
                            } else {
                                mecTransitionBuilder.addNextValue(currRow, column,  scalingFactor * entry.getValue());
                            }
                        }
                        if (!insertedDiagElement) {
                            mecTransitionBuilder.addNextValue(currRow, toSubModelStateMapping[mecState], selfLoopProb);
                        }
                        
                        // Compute the rewards obtained for this choice
                        choiceRewards.push_back(scalingFactor * rewardModel.getTotalStateActionReward(mecState, choice, transitionMatrix));
                        
                        ++currRow;
                    }
                }
                auto mecTransitions = mecTransitionBuilder.build();
                STORM_LOG_ASSERT(mecTransitions.isProbabilistic(), "The MEC-Matrix is not probabilistic.");
                
                // start the iterations
                ValueType precision = storm::utility::convertNumber<ValueType>(storm::settings::getModule<storm::settings::modules::MinMaxEquationSolverSettings>().getPrecision());
                std::vector<ValueType> x(mecTransitions.getRowGroupCount(), storm::utility::zero<ValueType>());
                std::vector<ValueType> xPrime = x;
                
                auto multiplier = storm::solver::MultiplierFactory<ValueType>().create(env, mecTransitions);
                ValueType maxDiff, minDiff;
                while (true) {
                    // Compute the obtained rewards for the next step
                    multiplier->multiplyAndReduce(env, dir, x, &choiceRewards, x);
                    
                    // update xPrime and check for convergence
                    // to avoid large (and numerically unstable) x-values, we substract a reference value.
                    auto xIt = x.begin();
                    auto xPrimeIt = xPrime.begin();
                    ValueType refVal = *xIt;
                    maxDiff = *xIt - *xPrimeIt;
                    minDiff = maxDiff;
                    *xIt -= refVal;
                    *xPrimeIt = *xIt;
                    for (++xIt, ++xPrimeIt; xIt != x.end(); ++xIt, ++xPrimeIt) {
                        ValueType diff = *xIt - *xPrimeIt;
                        maxDiff = std::max(maxDiff, diff);
                        minDiff = std::min(minDiff, diff);
                        *xIt -= refVal;
                        *xPrimeIt = *xIt;
                    }

                    if ((maxDiff - minDiff) < precision) {
                        break;
                    }
                }
                return (maxDiff + minDiff) / (storm::utility::convertNumber<ValueType>(2.0) * scalingFactor);
            }
            
            template<typename ValueType>
            template<typename RewardModelType>
            ValueType SparseMdpPrctlHelper<ValueType>::computeLraForMaximalEndComponentLP(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, RewardModelType const& rewardModel, storm::storage::MaximalEndComponent const& mec) {
                std::shared_ptr<storm::solver::LpSolver<ValueType>> solver = storm::utility::solver::getLpSolver<ValueType>("LRA for MEC");
                solver->setOptimizationDirection(invert(dir));
                
                // First, we need to create the variables for the problem.
                std::map<uint_fast64_t, storm::expressions::Variable> stateToVariableMap;
                for (auto const& stateChoicesPair : mec) {
                    std::string variableName = "h" + std::to_string(stateChoicesPair.first);
                    stateToVariableMap[stateChoicesPair.first] = solver->addUnboundedContinuousVariable(variableName);
                }
                storm::expressions::Variable lambda = solver->addUnboundedContinuousVariable("L", 1);
                solver->update();
                
                // Now we encode the problem as constraints.
                for (auto const& stateChoicesPair : mec) {
                    uint_fast64_t state = stateChoicesPair.first;
                    
                    // Now, based on the type of the state, create a suitable constraint.
                    for (auto choice : stateChoicesPair.second) {
                        storm::expressions::Expression constraint = -lambda;
                        
                        for (auto element : transitionMatrix.getRow(choice)) {
                            constraint = constraint + stateToVariableMap.at(element.getColumn()) * solver->getConstant(element.getValue());
                        }
                        typename RewardModelType::ValueType r = rewardModel.getTotalStateActionReward(state, choice, transitionMatrix);
                        constraint = solver->getConstant(r) + constraint;
                        
                        if (dir == OptimizationDirection::Minimize) {
                            constraint = stateToVariableMap.at(state) <= constraint;
                        } else {
                            constraint = stateToVariableMap.at(state) >= constraint;
                        }
                        solver->addConstraint("state" + std::to_string(state) + "," + std::to_string(choice), constraint);
                    }
                }
                
                solver->optimize();
                return solver->getContinuousValue(lambda);
            }
            
            template<typename ValueType>
            std::unique_ptr<CheckResult> SparseMdpPrctlHelper<ValueType>::computeConditionalProbabilities(Environment const& env, storm::solver::SolveGoal<ValueType>&& goal, storm::storage::SparseMatrix<ValueType> const& transitionMatrix, storm::storage::SparseMatrix<ValueType> const& backwardTransitions, storm::storage::BitVector const& targetStates, storm::storage::BitVector const& conditionStates) {
                
                std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
                
                // For the max-case, we can simply take the given target states. For the min-case, however, we need to
                // find the MECs of non-target states and make them the new target states.
                storm::storage::BitVector fixedTargetStates;
                if (!goal.minimize()) {
                    fixedTargetStates = targetStates;
                } else {
                    fixedTargetStates = storm::storage::BitVector(targetStates.size());
                    storm::storage::MaximalEndComponentDecomposition<ValueType> mecDecomposition(transitionMatrix, backwardTransitions, ~targetStates);
                    for (auto const& mec : mecDecomposition) {
                        for (auto const& stateActionsPair : mec) {
                            fixedTargetStates.set(stateActionsPair.first);
                        }
                    }
                }
                
                storm::storage::BitVector allStates(fixedTargetStates.size(), true);
                
                // Extend the target states by computing all states that have probability 1 to go to a target state
                // under *all* schedulers.
                fixedTargetStates = storm::utility::graph::performProb1A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, allStates, fixedTargetStates);
                
                // We solve the max-case and later adjust the result if the optimization direction was to minimize.
                storm::storage::BitVector initialStatesBitVector = goal.relevantValues();
                STORM_LOG_THROW(initialStatesBitVector.getNumberOfSetBits() == 1, storm::exceptions::NotSupportedException, "Computing conditional probabilities in MDPs is only supported for models with exactly one initial state.");
                storm::storage::sparse::state_type initialState = *initialStatesBitVector.begin();
                
                // Extend the condition states by computing all states that have probability 1 to go to a condition state
                // under *all* schedulers.
                storm::storage::BitVector extendedConditionStates = storm::utility::graph::performProb1A(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, allStates, conditionStates);

                STORM_LOG_DEBUG("Computing probabilities to satisfy condition.");
                std::chrono::high_resolution_clock::time_point conditionStart = std::chrono::high_resolution_clock::now();
                std::vector<ValueType> conditionProbabilities = std::move(computeUntilProbabilities(env, OptimizationDirection::Maximize, transitionMatrix, backwardTransitions, allStates, extendedConditionStates, false, false).values);
                std::chrono::high_resolution_clock::time_point conditionEnd = std::chrono::high_resolution_clock::now();
                STORM_LOG_DEBUG("Computed probabilities to satisfy for condition in " << std::chrono::duration_cast<std::chrono::milliseconds>(conditionEnd - conditionStart).count() << "ms.");
                
                // If the conditional probability is undefined for the initial state, we return directly.
                if (storm::utility::isZero(conditionProbabilities[initialState])) {
                    return std::unique_ptr<CheckResult>(new ExplicitQuantitativeCheckResult<ValueType>(initialState, storm::utility::infinity<ValueType>()));
                }
                
                STORM_LOG_DEBUG("Computing probabilities to reach target.");
                std::chrono::high_resolution_clock::time_point targetStart = std::chrono::high_resolution_clock::now();
                std::vector<ValueType> targetProbabilities = std::move(computeUntilProbabilities(env, OptimizationDirection::Maximize, transitionMatrix, backwardTransitions, allStates, fixedTargetStates, false, false).values);
                std::chrono::high_resolution_clock::time_point targetEnd = std::chrono::high_resolution_clock::now();
                STORM_LOG_DEBUG("Computed probabilities to reach target in " << std::chrono::duration_cast<std::chrono::milliseconds>(targetEnd - targetStart).count() << "ms.");

                storm::storage::BitVector statesWithProbabilityGreater0E(transitionMatrix.getRowGroupCount(), true);
                storm::storage::sparse::state_type state = 0;
                for (auto const& element : conditionProbabilities) {
                    if (storm::utility::isZero(element)) {
                        statesWithProbabilityGreater0E.set(state, false);
                    }
                    ++state;
                }

                // Determine those states that need to be equipped with a restart mechanism.
                STORM_LOG_DEBUG("Computing problematic states.");
                storm::storage::BitVector pureResetStates = storm::utility::graph::performProb0A(backwardTransitions, allStates, extendedConditionStates);
                storm::storage::BitVector problematicStates = storm::utility::graph::performProb0E(transitionMatrix, transitionMatrix.getRowGroupIndices(), backwardTransitions, allStates, extendedConditionStates | fixedTargetStates);

                // Otherwise, we build the transformed MDP.
                storm::storage::BitVector relevantStates = storm::utility::graph::getReachableStates(transitionMatrix, initialStatesBitVector, allStates, extendedConditionStates | fixedTargetStates | pureResetStates);
                STORM_LOG_TRACE("Found " << relevantStates.getNumberOfSetBits() << " relevant states for conditional probability computation.");
                std::vector<uint_fast64_t> numberOfStatesBeforeRelevantStates = relevantStates.getNumberOfSetBitsBeforeIndices();
                storm::storage::sparse::state_type newGoalState = relevantStates.getNumberOfSetBits();
                storm::storage::sparse::state_type newStopState = newGoalState + 1;
                storm::storage::sparse::state_type newFailState = newStopState + 1;
                
                // Build the transitions of the (relevant) states of the original model.
                storm::storage::SparseMatrixBuilder<ValueType> builder(0, newFailState + 1, 0, true, true);
                uint_fast64_t currentRow = 0;
                for (auto state : relevantStates) {
                    builder.newRowGroup(currentRow);
                    if (fixedTargetStates.get(state)) {
                        if (!storm::utility::isZero(conditionProbabilities[state])) {
                            builder.addNextValue(currentRow, newGoalState, conditionProbabilities[state]);
                        }
                        if (!storm::utility::isOne(conditionProbabilities[state])) {
                            builder.addNextValue(currentRow, newFailState, storm::utility::one<ValueType>() - conditionProbabilities[state]);
                        }
                        ++currentRow;
                    } else if (extendedConditionStates.get(state)) {
                        if (!storm::utility::isZero(targetProbabilities[state])) {
                            builder.addNextValue(currentRow, newGoalState, targetProbabilities[state]);
                        }
                        if (!storm::utility::isOne(targetProbabilities[state])) {
                            builder.addNextValue(currentRow, newStopState, storm::utility::one<ValueType>() - targetProbabilities[state]);
                        }
                        ++currentRow;
                    } else if (pureResetStates.get(state)) {
                        builder.addNextValue(currentRow, numberOfStatesBeforeRelevantStates[initialState], storm::utility::one<ValueType>());
                        ++currentRow;
                    } else {
                        for (uint_fast64_t row = transitionMatrix.getRowGroupIndices()[state]; row < transitionMatrix.getRowGroupIndices()[state + 1]; ++row) {
                            for (auto const& successorEntry : transitionMatrix.getRow(row)) {
                                builder.addNextValue(currentRow, numberOfStatesBeforeRelevantStates[successorEntry.getColumn()], successorEntry.getValue());
                            }
                            ++currentRow;
                        }
                        if (problematicStates.get(state)) {
                            builder.addNextValue(currentRow, numberOfStatesBeforeRelevantStates[initialState], storm::utility::one<ValueType>());
                            ++currentRow;
                        }
                    }
                }
                
                // Now build the transitions of the newly introduced states.
                builder.newRowGroup(currentRow);
                builder.addNextValue(currentRow, newGoalState, storm::utility::one<ValueType>());
                ++currentRow;
                builder.newRowGroup(currentRow);
                builder.addNextValue(currentRow, newStopState, storm::utility::one<ValueType>());
                ++currentRow;
                builder.newRowGroup(currentRow);
                builder.addNextValue(currentRow, numberOfStatesBeforeRelevantStates[initialState], storm::utility::one<ValueType>());
                ++currentRow;
                
                std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
                STORM_LOG_DEBUG("Computed transformed model in " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms.");
                
                // Finally, build the matrix and dispatch the query as a reachability query.
                STORM_LOG_DEBUG("Computing conditional probabilties.");
                storm::storage::BitVector newGoalStates(newFailState + 1);
                newGoalStates.set(newGoalState);
                storm::storage::SparseMatrix<ValueType> newTransitionMatrix = builder.build();
                STORM_LOG_DEBUG("Transformed model has " << newTransitionMatrix.getRowGroupCount() << " states and " << newTransitionMatrix.getNonzeroEntryCount() << " transitions.");
                storm::storage::SparseMatrix<ValueType> newBackwardTransitions = newTransitionMatrix.transpose(true);
                
                storm::solver::OptimizationDirection dir = goal.direction();
                if (goal.minimize()) {
                    goal.oneMinus();
                }
                
                std::chrono::high_resolution_clock::time_point conditionalStart = std::chrono::high_resolution_clock::now();
                std::vector<ValueType> goalProbabilities = std::move(computeUntilProbabilities(env, std::move(goal), newTransitionMatrix, newBackwardTransitions, storm::storage::BitVector(newFailState + 1, true), newGoalStates, false, false).values);
                std::chrono::high_resolution_clock::time_point conditionalEnd = std::chrono::high_resolution_clock::now();
                STORM_LOG_DEBUG("Computed conditional probabilities in transformed model in " << std::chrono::duration_cast<std::chrono::milliseconds>(conditionalEnd - conditionalStart).count() << "ms.");
                
                return std::unique_ptr<CheckResult>(new ExplicitQuantitativeCheckResult<ValueType>(initialState, dir == OptimizationDirection::Maximize ? goalProbabilities[numberOfStatesBeforeRelevantStates[initialState]] : storm::utility::one<ValueType>() - goalProbabilities[numberOfStatesBeforeRelevantStates[initialState]]));
            }
            
            template class SparseMdpPrctlHelper<double>;
            template std::vector<double> SparseMdpPrctlHelper<double>::computeInstantaneousRewards(Environment const& env, storm::solver::SolveGoal<double>&& goal, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::models::sparse::StandardRewardModel<double> const& rewardModel, uint_fast64_t stepCount);
            template std::vector<double> SparseMdpPrctlHelper<double>::computeCumulativeRewards(Environment const& env, storm::solver::SolveGoal<double>&& goal, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::models::sparse::StandardRewardModel<double> const& rewardModel, uint_fast64_t stepBound);
            template MDPSparseModelCheckingHelperReturnType<double> SparseMdpPrctlHelper<double>::computeReachabilityRewards(Environment const& env, storm::solver::SolveGoal<double>&& goal, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::storage::SparseMatrix<double> const& backwardTransitions, storm::models::sparse::StandardRewardModel<double> const& rewardModel, storm::storage::BitVector const& targetStates, bool qualitative, bool produceScheduler, ModelCheckerHint const& hint);
            template std::vector<double> SparseMdpPrctlHelper<double>::computeLongRunAverageRewards(Environment const& env, storm::solver::SolveGoal<double>&& goal, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::storage::SparseMatrix<double> const& backwardTransitions, storm::models::sparse::StandardRewardModel<double> const& rewardModel);
            template double SparseMdpPrctlHelper<double>::computeLraForMaximalEndComponent(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::models::sparse::StandardRewardModel<double> const& rewardModel, storm::storage::MaximalEndComponent const& mec);
            template double SparseMdpPrctlHelper<double>::computeLraForMaximalEndComponentVI(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::models::sparse::StandardRewardModel<double> const& rewardModel, storm::storage::MaximalEndComponent const& mec);
            template double SparseMdpPrctlHelper<double>::computeLraForMaximalEndComponentLP(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<double> const& transitionMatrix, storm::models::sparse::StandardRewardModel<double> const& rewardModel, storm::storage::MaximalEndComponent const& mec);

#ifdef STORM_HAVE_CARL
            template class SparseMdpPrctlHelper<storm::RationalNumber>;
            template std::vector<storm::RationalNumber> SparseMdpPrctlHelper<storm::RationalNumber>::computeInstantaneousRewards(Environment const& env, storm::solver::SolveGoal<storm::RationalNumber>&& goal, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, uint_fast64_t stepCount);
            template std::vector<storm::RationalNumber> SparseMdpPrctlHelper<storm::RationalNumber>::computeCumulativeRewards(Environment const& env, storm::solver::SolveGoal<storm::RationalNumber>&& goal, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, uint_fast64_t stepBound);
            template MDPSparseModelCheckingHelperReturnType<storm::RationalNumber> SparseMdpPrctlHelper<storm::RationalNumber>::computeReachabilityRewards(Environment const& env, storm::solver::SolveGoal<storm::RationalNumber>&& goal, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::storage::SparseMatrix<storm::RationalNumber> const& backwardTransitions, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, storm::storage::BitVector const& targetStates, bool qualitative, bool produceScheduler, ModelCheckerHint const& hint);
            template std::vector<storm::RationalNumber> SparseMdpPrctlHelper<storm::RationalNumber>::computeLongRunAverageRewards(Environment const& env, storm::solver::SolveGoal<storm::RationalNumber>&& goal, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::storage::SparseMatrix<storm::RationalNumber> const& backwardTransitions, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel);
            template storm::RationalNumber SparseMdpPrctlHelper<storm::RationalNumber>::computeLraForMaximalEndComponent(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, storm::storage::MaximalEndComponent const& mec);
            template storm::RationalNumber SparseMdpPrctlHelper<storm::RationalNumber>::computeLraForMaximalEndComponentVI(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, storm::storage::MaximalEndComponent const& mec);
            template storm::RationalNumber SparseMdpPrctlHelper<storm::RationalNumber>::computeLraForMaximalEndComponentLP(Environment const& env, OptimizationDirection dir, storm::storage::SparseMatrix<storm::RationalNumber> const& transitionMatrix, storm::models::sparse::StandardRewardModel<storm::RationalNumber> const& rewardModel, storm::storage::MaximalEndComponent const& mec);
#endif
        }
    }
}
