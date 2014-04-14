#include "Update.h"
#include "src/exceptions/ExceptionMacros.h"
#include "src/exceptions/OutOfRangeException.h"

namespace storm {
    namespace prism {
        Update::Update(uint_fast64_t globalIndex, storm::expressions::Expression const& likelihoodExpression, std::vector<storm::prism::Assignment> const& assignments, std::string const& filename, uint_fast64_t lineNumber) : LocatedInformation(filename, lineNumber), likelihoodExpression(likelihoodExpression), assignments(assignments), variableToAssignmentIndexMap(), globalIndex(globalIndex) {
            this->createAssignmentMapping();
        }
        
        storm::expressions::Expression const& Update::getLikelihoodExpression() const {
            return likelihoodExpression;
        }
        
        std::size_t Update::getNumberOfAssignments() const {
            return this->assignments.size();
        }
        
        std::vector<storm::prism::Assignment> const& Update::getAssignments() const {
            return this->assignments;
        }
        
        storm::prism::Assignment const& Update::getAssignment(std::string const& variableName) const {
            auto const& variableIndexPair = this->variableToAssignmentIndexMap.find(variableName);
            LOG_THROW(variableIndexPair != this->variableToAssignmentIndexMap.end(), storm::exceptions::OutOfRangeException, "Variable '" << variableName << "' is not assigned in update.");
            return this->getAssignments()[variableIndexPair->second];
        }
        
        uint_fast64_t Update::getGlobalIndex() const {
            return this->globalIndex;
        }
        
        void Update::createAssignmentMapping() {
            this->variableToAssignmentIndexMap.clear();
            for (uint_fast64_t assignmentIndex = 0; assignmentIndex < this->getAssignments().size(); ++assignmentIndex) {
                this->variableToAssignmentIndexMap[this->getAssignments()[assignmentIndex].getVariableName()] = assignmentIndex;
            }
        }
        
        Update Update::substitute(std::map<std::string, storm::expressions::Expression> const& substitution) const {
            std::vector<Assignment> newAssignments;
            newAssignments.reserve(this->getNumberOfAssignments());
            for (auto const& assignment : this->getAssignments()) {
                newAssignments.emplace_back(assignment.substitute(substitution));
            }
            
            return Update(this->getGlobalIndex(), this->getLikelihoodExpression().substitute<std::map>(substitution), newAssignments, this->getFilename(), this->getLineNumber());
        }
        
        std::ostream& operator<<(std::ostream& stream, Update const& update) {
            stream << update.getLikelihoodExpression() << " : ";
            uint_fast64_t i = 0;
            for (auto const& assignment : update.getAssignments()) {
                stream << assignment;
                if (i < update.getAssignments().size() - 1) {
                    stream << " & ";
                }
                ++i;
            }
            return stream;
        }
        
    } // namespace ir
} // namespace storm
