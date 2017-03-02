#pragma once

#include "storm/logic/Formulas.h"
#include "storm/modelchecker/CheckTask.h"
#include "storm/modelchecker/results/CheckResult.h"
#include "storm/modelchecker/hints/ModelCheckerHint.h"
#include "storm/utility/parametric.h"

namespace storm {
    namespace modelchecker {
        namespace parametric {
            
            /*!
             * Class to efficiently check a formula on a parametric model with different parameter instantiations
             */
            template <typename SparseModelType, typename ConstantType>
            class SparseInstantiationModelChecker {
            public:
                SparseInstantiationModelChecker(SparseModelType const& parametricModel);
                
                void specifyFormula(CheckTask<storm::logic::Formula, typename SparseModelType::ValueType> const& checkTask);
                
                virtual std::unique_ptr<CheckResult> check(storm::utility::parametric::Valuation<typename SparseModelType::ValueType> const& valuation) = 0;
                
                storm::modelchecker::ModelCheckerHint& getHint();
                storm::modelchecker::ModelCheckerHint const& getHint() const;
                
            protected:
                
                SparseModelType const& parametricModel;
                std::unique_ptr<CheckTask<storm::logic::Formula, ConstantType>> currentCheckTask;

            private:
                // store the current formula. Note that currentCheckTask only stores a reference to the formula.
                std::shared_ptr<storm::logic::Formula const> currentFormula;

            };
        }
    }
}
