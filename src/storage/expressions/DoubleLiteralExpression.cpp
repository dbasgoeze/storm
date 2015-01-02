#include "src/storage/expressions/DoubleLiteralExpression.h"
#include "src/storage/expressions/ExpressionManager.h"

namespace storm {
    namespace expressions {
        DoubleLiteralExpression::DoubleLiteralExpression(ExpressionManager const& manager, double value) : BaseExpression(manager, manager.getRationalType()), value(value) {
            // Intentionally left empty.
        }
        
        double DoubleLiteralExpression::evaluateAsDouble(Valuation const* valuation) const {
            return this->getValue();
        }
        
        bool DoubleLiteralExpression::isLiteral() const {
            return true;
        }
        
        std::set<std::string> DoubleLiteralExpression::getVariables() const {
            return std::set<std::string>();
		}
        
        std::shared_ptr<BaseExpression const> DoubleLiteralExpression::simplify() const {
            return this->shared_from_this();
        }
        
        boost::any DoubleLiteralExpression::accept(ExpressionVisitor& visitor) const {
            return visitor.visit(*this);
        }
        
        double DoubleLiteralExpression::getValue() const {
            return this->value;
        }
        
        void DoubleLiteralExpression::printToStream(std::ostream& stream) const {
            stream << this->getValue();
        }
    }
}