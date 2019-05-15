//
// Created by Jip Spel on 19.09.18.
//

// TODO: cleanup includes
#include "gtest/gtest.h"
#include "storm-config.h"
#include "test/storm_gtest.h"

#include "storm-parsers/parser/FormulaParser.h"
#include "storm/logic/Formulas.h"
#include "storm/models/sparse/StandardRewardModel.h"
#include "storm/modelchecker/prctl/SparseDtmcPrctlModelChecker.h"
#include "storm/modelchecker/results/ExplicitQuantitativeCheckResult.h"
#include "storm-parsers/parser/AutoParser.h"
#include "storm-parsers/parser/PrismParser.h"
#include "storm/storage/expressions/ExpressionManager.h"
#include "storm/api/builder.h"

#include "storm-pars/analysis/AssumptionChecker.h"
#include "storm-pars/analysis/Lattice.h"
#include "storm/storage/expressions/BinaryRelationExpression.h"
#include "storm-pars/transformer/SparseParametricDtmcSimplifier.h"

#include "storm-pars/api/storm-pars.h"
#include "storm/api/storm.h"

#include "storm-parsers/api/storm-parsers.h"

TEST(AssumptionCheckerTest, Brp_no_bisimulation) {
    std::string programFile = STORM_TEST_RESOURCES_DIR "/pdtmc/brp16_2.pm";
    std::string formulaAsString = "P=? [F s=4 & i=N ]";
    std::string constantsAsString = ""; //e.g. pL=0.9,TOACK=0.5

    // Program and formula
    storm::prism::Program program = storm::api::parseProgram(programFile);
    program = storm::utility::prism::preprocess(program, constantsAsString);
    std::vector<std::shared_ptr<const storm::logic::Formula>> formulas = storm::api::extractFormulasFromProperties(storm::api::parsePropertiesForPrismProgram(formulaAsString, program));
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> model = storm::api::buildSparseModel<storm::RationalFunction>(program, formulas)->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    auto simplifier = storm::transformer::SparseParametricDtmcSimplifier<storm::models::sparse::Dtmc<storm::RationalFunction>>(*dtmc);
    ASSERT_TRUE(simplifier.simplify(*(formulas[0])));
    model = simplifier.getSimplifiedModel();

    dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();

    ASSERT_EQ(dtmc->getNumberOfStates(), 193ull);
    ASSERT_EQ(dtmc->getNumberOfTransitions(), 383ull);

    auto checker = storm::analysis::AssumptionChecker<storm::RationalFunction>(formulas[0], dtmc, 3);

    // Check on samples
    auto expressionManager = std::make_shared<storm::expressions::ExpressionManager>(storm::expressions::ExpressionManager());
    expressionManager->declareRationalVariable("7");
    expressionManager->declareRationalVariable("5");

    auto assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("7").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("5").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::UNKNOWN, checker.checkOnSamples(assumption));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("5").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("7").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));


    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("7").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("5").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Equal));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));


    storm::storage::BitVector above(8);
    above.set(0);
    storm::storage::BitVector below(8);
    below.set(1);
    storm::storage::BitVector initialMiddle(8);

    auto dummyLattice = new storm::analysis::Lattice(&above, &below, &initialMiddle, 8);
    // Validate assumption
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, dummyLattice));
//    EXPECT_FALSE(checker.validated(assumption));

    expressionManager->declareRationalVariable("6");
    expressionManager->declareRationalVariable("8");
    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("6").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("8").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    above = storm::storage::BitVector(13);
    above.set(12);
    below = storm::storage::BitVector(13);
    below.set(9);
    initialMiddle = storm::storage::BitVector(13);
    dummyLattice = new storm::analysis::Lattice(&above, &below, &initialMiddle, 13);
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, dummyLattice));
//    EXPECT_EQ(checker.validated(assumption));
//    EXPECT_FALSE(checker.valid(assumption));
}

TEST(AssumptionCheckerTest, Simple1) {
    std::string programFile = STORM_TEST_RESOURCES_DIR "/pdtmc/simple1.pm";
    std::string formulaAsString = "P=? [F s=3]";
    std::string constantsAsString = ""; //e.g. pL=0.9,TOACK=0.5

    // Program and formula
    storm::prism::Program program = storm::api::parseProgram(programFile);
    program = storm::utility::prism::preprocess(program, constantsAsString);
    std::vector<std::shared_ptr<const storm::logic::Formula>> formulas = storm::api::extractFormulasFromProperties(storm::api::parsePropertiesForPrismProgram(formulaAsString, program));
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> model = storm::api::buildSparseModel<storm::RationalFunction>(program, formulas)->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    auto simplifier = storm::transformer::SparseParametricDtmcSimplifier<storm::models::sparse::Dtmc<storm::RationalFunction>>(*dtmc);
    ASSERT_TRUE(simplifier.simplify(*(formulas[0])));
    model = simplifier.getSimplifiedModel();
    dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();

    ASSERT_EQ(dtmc->getNumberOfStates(), 5);
    ASSERT_EQ(dtmc->getNumberOfTransitions(), 8);

    auto checker = storm::analysis::AssumptionChecker<storm::RationalFunction>(formulas[0], dtmc, 3);

    auto expressionManager = std::make_shared<storm::expressions::ExpressionManager>(storm::expressions::ExpressionManager());
    expressionManager->declareRationalVariable("1");
    expressionManager->declareRationalVariable("2");

    // Checking on samples
    auto assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Equal));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
}

TEST(AssumptionCheckerTest, Simple2) {
    std::string programFile = STORM_TEST_RESOURCES_DIR "/pdtmc/simple2.pm";
    std::string formulaAsString = "P=? [F s=3]";
    std::string constantsAsString = ""; //e.g. pL=0.9,TOACK=0.5

    // Program and formula
    storm::prism::Program program = storm::api::parseProgram(programFile);
    program = storm::utility::prism::preprocess(program, constantsAsString);
    std::vector<std::shared_ptr<const storm::logic::Formula>> formulas = storm::api::extractFormulasFromProperties(storm::api::parsePropertiesForPrismProgram(formulaAsString, program));
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> model = storm::api::buildSparseModel<storm::RationalFunction>(program, formulas)->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    auto simplifier = storm::transformer::SparseParametricDtmcSimplifier<storm::models::sparse::Dtmc<storm::RationalFunction>>(*dtmc);
    ASSERT_TRUE(simplifier.simplify(*(formulas[0])));
    model = simplifier.getSimplifiedModel();
    dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();

    ASSERT_EQ(dtmc->getNumberOfStates(), 5);
    ASSERT_EQ(dtmc->getNumberOfTransitions(), 8);

    auto checker = storm::analysis::AssumptionChecker<storm::RationalFunction>(formulas[0], dtmc, 3);

    auto expressionManager = std::make_shared<storm::expressions::ExpressionManager>(storm::expressions::ExpressionManager());
    expressionManager->declareRationalVariable("1");
    expressionManager->declareRationalVariable("2");

    storm::storage::BitVector above(5);
    above.set(3);
    storm::storage::BitVector below(5);
    below.set(4);
    storm::storage::BitVector initialMiddle(5);

    auto lattice = new storm::analysis::Lattice(&above, &below, &initialMiddle, 5);

    // Checking on samples and validate
    auto assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::UNKNOWN, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::VALID, checker.validateAssumption(assumption, lattice));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, lattice));


    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Equal));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, lattice));
}

TEST(AssumptionCheckerTest, Simple3) {
    std::string programFile = STORM_TEST_RESOURCES_DIR "/pdtmc/simple3.pm";
    std::string formulaAsString = "P=? [F s=4]";
    std::string constantsAsString = ""; //e.g. pL=0.9,TOACK=0.5

    // Program and formula
    storm::prism::Program program = storm::api::parseProgram(programFile);
    program = storm::utility::prism::preprocess(program, constantsAsString);
    std::vector<std::shared_ptr<const storm::logic::Formula>> formulas = storm::api::extractFormulasFromProperties(storm::api::parsePropertiesForPrismProgram(formulaAsString, program));
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> model = storm::api::buildSparseModel<storm::RationalFunction>(program, formulas)->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    std::shared_ptr<storm::models::sparse::Dtmc<storm::RationalFunction>> dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();
    auto simplifier = storm::transformer::SparseParametricDtmcSimplifier<storm::models::sparse::Dtmc<storm::RationalFunction>>(*dtmc);
    ASSERT_TRUE(simplifier.simplify(*(formulas[0])));
    model = simplifier.getSimplifiedModel();
    dtmc = model->as<storm::models::sparse::Dtmc<storm::RationalFunction>>();

    ASSERT_EQ(6, dtmc->getNumberOfStates());
    ASSERT_EQ(12, dtmc->getNumberOfTransitions());

    auto checker = storm::analysis::AssumptionChecker<storm::RationalFunction>(formulas[0], dtmc, 3);

    auto expressionManager = std::make_shared<storm::expressions::ExpressionManager>(storm::expressions::ExpressionManager());
    expressionManager->declareRationalVariable("1");
    expressionManager->declareRationalVariable("2");

    storm::storage::BitVector above(6);
    above.set(4);
    storm::storage::BitVector below(6);
    below.set(5);
    storm::storage::BitVector initialMiddle(6);

    auto lattice = new storm::analysis::Lattice(&above, &below, &initialMiddle, 6);
    lattice->add(3);

    // Checking on samples and validate
    auto assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::UNKNOWN, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::VALID, checker.validateAssumption(assumption, lattice));
    EXPECT_EQ(storm::analysis::AssumptionStatus::VALID, checker.validateAssumptionSMTSolver(assumption, lattice));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Greater));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, lattice));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumptionSMTSolver(assumption, lattice));

    assumption = std::make_shared<storm::expressions::BinaryRelationExpression>(
            storm::expressions::BinaryRelationExpression(*expressionManager, expressionManager->getBooleanType(),
                                                         expressionManager->getVariable("1").getExpression().getBaseExpressionPointer(),
                                                         expressionManager->getVariable("2").getExpression().getBaseExpressionPointer(),
                                                         storm::expressions::BinaryRelationExpression::RelationType::Equal));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.checkOnSamples(assumption));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumption(assumption, lattice));
    EXPECT_EQ(storm::analysis::AssumptionStatus::INVALID, checker.validateAssumptionSMTSolver(assumption, lattice));
}


