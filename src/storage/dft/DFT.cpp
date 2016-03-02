#include <boost/container/flat_set.hpp>

#include <map>
#include "DFT.h"
#include "DFTBuilder.h"
#include "src/exceptions/NotSupportedException.h"

#include "DFTIsomorphism.h"
#include "utility/iota_n.h"

namespace storm {
    namespace storage {

        template<typename ValueType>
        DFT<ValueType>::DFT(DFTElementVector const& elements, DFTElementPointer const& tle) : mElements(elements), mNrOfBEs(0), mNrOfSpares(0), mTopLevelIndex(tle->id()), mMaxSpareChildCount(0) {
            assert(elementIndicesCorrect());
            size_t nrRepresentatives = 0;
            
            for (auto& elem : mElements) {
                if (isRepresentative(elem->id())) {
                    ++nrRepresentatives;
                }
                if(elem->isBasicElement()) {
                    ++mNrOfBEs;
                }
                else if (elem->isSpareGate()) {
                    ++mNrOfSpares;
                    bool firstChild = true;
                    mMaxSpareChildCount = std::max(mMaxSpareChildCount, std::static_pointer_cast<DFTSpare<ValueType>>(elem)->children().size());
                    for(auto const& spareReprs : std::static_pointer_cast<DFTSpare<ValueType>>(elem)->children()) {
                        std::set<size_t> module = {spareReprs->id()};
                        spareReprs->extendSpareModule(module);
                        std::vector<size_t> sparesAndBes;
                        for(size_t modelem : module) {
                            if (spareReprs->id() != modelem && (isRepresentative(modelem) || (!firstChild && mTopLevelIndex == modelem))) {
                                STORM_LOG_THROW(false, storm::exceptions::NotSupportedException, "Module for '" << spareReprs->name() << "' contains more than one representative.");
                            }
                            if(mElements[modelem]->isSpareGate() || mElements[modelem]->isBasicElement()) {
                                sparesAndBes.push_back(modelem);
                                mRepresentants.insert(std::make_pair(modelem, spareReprs->id()));
                            }
                        }
                        mSpareModules.insert(std::make_pair(spareReprs->id(), sparesAndBes));
                        firstChild = false;
                    }

                } else if (elem->isDependency()) {
                    mDependencies.push_back(elem->id());
                }
            }
          
            // For the top module, we assume, contrary to [Jun15], that we have all spare gates and basic elements which are not in another module.
            std::set<size_t> topModuleSet;
            // Initialize with all ids.
            for(auto const& elem : mElements) {
                if (elem->isBasicElement() || elem->isSpareGate()) {
                    topModuleSet.insert(elem->id());
                }
            }
            // Erase spare modules
            for(auto const& module : mSpareModules) {
                for(auto const& index : module.second) {
                    topModuleSet.erase(index);
                }
            }
            // Extend top module and insert those elements which are part of the top module and a spare module
            mElements[mTopLevelIndex]->extendSpareModule(topModuleSet);
            mTopModule = std::vector<size_t>(topModuleSet.begin(), topModuleSet.end());
            // Clear all spare modules where at least one element is also in the top module
            if (!mTopModule.empty()) {
                for (auto& module : mSpareModules) {
                    if (std::find(module.second.begin(), module.second.end(), mTopModule.front()) != module.second.end()) {
                        module.second.clear();
                    }
                }
            }

            //Reserve space for failed spares
            ++mMaxSpareChildCount;
            size_t usageInfoBits = storm::utility::math::uint64_log2(mMaxSpareChildCount) + 1;
            mStateVectorSize = nrElements() * 2 + mNrOfSpares * usageInfoBits + nrRepresentatives;
        }

        template<typename ValueType>
        DFTStateGenerationInfo DFT<ValueType>::buildStateGenerationInfo(storm::storage::DFTIndependentSymmetries const& symmetries) const {
            // Use symmetry
            // Collect all elements in the first subtree
            // TODO make recursive to use for nested subtrees

            DFTStateGenerationInfo generationInfo(nrElements(), mMaxSpareChildCount);
            
            // Generate Pre and Post info for restrictions
            for(auto const& elem : mElements) {
                if(!elem->isDependency() && !elem->isRestriction()) {
                    generationInfo.setRestrictionPreElements(elem->id(), elem->seqRestrictionPres());
                    generationInfo.setRestrictionPostElements(elem->id(), elem->seqRestrictionPosts());
                }
            }
            

            // Perform DFS and insert all elements of subtree sequentially
            size_t stateIndex = 0;
            std::queue<size_t> visitQueue;
            storm::storage::BitVector visited(nrElements(), false);

            // TODO make subfunction for this?
            if (symmetries.groups.empty()) {
                // Perform DFS for whole tree
                visitQueue.push(mTopLevelIndex);
                stateIndex = performStateGenerationInfoDFS(generationInfo, visitQueue, visited, stateIndex);
            } else {
                for (auto const& symmetryGroup : symmetries.groups) {
                    assert(!symmetryGroup.second.empty());

                    // Perform DFS for first subtree of each symmetry
                    visitQueue.push(symmetryGroup.first);
                    size_t groupIndex = stateIndex;
                    stateIndex = performStateGenerationInfoDFS(generationInfo, visitQueue, visited, stateIndex);
                    size_t offset = stateIndex - groupIndex;
                    
                    // Mirror symmetries
                    size_t noSymmetricElements = symmetryGroup.second.front().size();
                    assert(noSymmetricElements > 1);

                    for (std::vector<size_t> symmetricElements : symmetryGroup.second) {
                        assert(symmetricElements.size() == noSymmetricElements);

                        // Initialize for original element
                        size_t originalElement = symmetricElements[0];
                        size_t index = generationInfo.getStateIndex(originalElement);
                        size_t activationIndex = isRepresentative(originalElement) ? generationInfo.getSpareActivationIndex(originalElement) : 0;
                        size_t usageIndex = mElements[originalElement]->isSpareGate() ? generationInfo.getSpareUsageIndex(originalElement) : 0;
                        
                        // Mirror symmetry for each element
                        for (size_t i = 1; i < symmetricElements.size(); ++i) {
                            size_t symmetricElement = symmetricElements[i];
                            visited.set(symmetricElement);

                            generationInfo.addStateIndex(symmetricElement, index + offset * i);
                            stateIndex += 2;

                            assert((activationIndex > 0) == isRepresentative(symmetricElement));
                            if (activationIndex > 0) {
                                generationInfo.addSpareActivationIndex(symmetricElement, activationIndex + offset * i);
                                ++stateIndex;
                            }

                            assert((usageIndex > 0) == mElements[symmetricElement]->isSpareGate());
                            if (usageIndex > 0) {
                                generationInfo.addSpareUsageIndex(symmetricElement, usageIndex + offset * i);
                                stateIndex += generationInfo.usageInfoBits();
                            }
                        }
                    }

                    // Store starting indices of symmetry groups
                    std::vector<size_t> symmetryIndices;
                    for (size_t i = 0; i < noSymmetricElements; ++i) {
                        symmetryIndices.push_back(groupIndex + i * offset);
                    }
                    generationInfo.addSymmetry(offset, symmetryIndices);
                }
            }

            // TODO symmetries in dependencies?
            // Consider dependencies
            for (size_t idDependency : getDependencies()) {
                std::shared_ptr<DFTDependency<ValueType> const> dependency = getDependency(idDependency);
                visitQueue.push(dependency->id());
                visitQueue.push(dependency->triggerEvent()->id());
                visitQueue.push(dependency->dependentEvent()->id());
            }
            stateIndex = performStateGenerationInfoDFS(generationInfo, visitQueue, visited, stateIndex);

            // Visit all remaining states
            for (size_t i = 0; i < visited.size(); ++i) {
                if (!visited[i]) {
                    visitQueue.push(i);
                    stateIndex = performStateGenerationInfoDFS(generationInfo, visitQueue, visited, stateIndex);
                }
            }

            STORM_LOG_TRACE(generationInfo);
            assert(stateIndex == mStateVectorSize);
            assert(visited.full());
            
            return generationInfo;
        }
        
        template<typename ValueType>
        size_t DFT<ValueType>::generateStateInfo(DFTStateGenerationInfo& generationInfo, size_t id, storm::storage::BitVector& visited, size_t stateIndex) const {
            assert(!visited[id]);
            visited.set(id);
            
            // Reserve bits for element
            generationInfo.addStateIndex(id, stateIndex);
            stateIndex += 2;
            
            if (isRepresentative(id)) {
                generationInfo.addSpareActivationIndex(id, stateIndex);
                ++stateIndex;
            }
            
            if (mElements[id]->isSpareGate()) {
                generationInfo.addSpareUsageIndex(id, stateIndex);
                stateIndex += generationInfo.usageInfoBits();
            }
            
            return stateIndex;
        }
        
        template<typename ValueType>
        size_t DFT<ValueType>::performStateGenerationInfoDFS(DFTStateGenerationInfo& generationInfo, std::queue<size_t>& visitQueue, storm::storage::BitVector& visited, size_t stateIndex) const {
            while (!visitQueue.empty()) {
                size_t id = visitQueue.front();
                visitQueue.pop();
                if (visited[id]) {
                    // Already visited
                    continue;
                }
                stateIndex = generateStateInfo(generationInfo, id, visited, stateIndex);
                
                // Insert children
                if (mElements[id]->isGate()) {
                    for (auto const& child : std::static_pointer_cast<DFTGate<ValueType>>(mElements[id])->children()) {
                        visitQueue.push(child->id());
                    }
                }
            }
            return stateIndex;
        }
        
        template<typename ValueType>
        DFT<ValueType> DFT<ValueType>::optimize() const {
            std::vector<size_t> modIdea = findModularisationRewrite();
            std::cout << "Modularisation idea: " << std::endl;
    
            for( auto const& i  : modIdea ) {
                std::cout << i << ", ";
            }

            if (modIdea.empty()) {
                // No rewrite needed
                return *this;
            }
            
            std::vector<std::vector<size_t>> rewriteIds;
            rewriteIds.push_back(modIdea);
            
            DFTBuilder<ValueType> builder;
            
            // Accumulate elements which must be rewritten
            std::set<size_t> rewriteSet;
            for (std::vector<size_t> rewrites : rewriteIds) {
                rewriteSet.insert(rewrites.front());
            }
            // Copy all other elements which do not change
            for (auto elem : mElements) {
                if (rewriteSet.count(elem->id()) == 0) {
                    builder.copyElement(elem);
                }
            }
            
            // Add rewritten elements
            for (std::vector<size_t> rewrites : rewriteIds) {
                assert(rewrites.size() > 1);
                assert(mElements[rewrites[1]]->hasParents());
                assert(mElements[rewrites[1]]->parents().front()->isGate());
                DFTGatePointer originalParent = std::static_pointer_cast<DFTGate<ValueType>>(mElements[rewrites[1]]->parents().front());
                std::string newParentName = builder.getUniqueName(originalParent->name());
                
                // Accumulate children names
                std::vector<std::string> childrenNames;
                for (size_t i = 1; i < rewrites.size(); ++i) {
                    assert(mElements[rewrites[i]]->parents().front()->id() == originalParent->id()); // Children have the same father
                    childrenNames.push_back(mElements[rewrites[i]]->name());
                }
                
                // Add element inbetween parent and children
                switch (originalParent->type()) {
                    case DFTElementType::AND:
                        builder.addAndElement(newParentName, childrenNames);
                        break;
                    case DFTElementType::OR:
                        builder.addOrElement(newParentName, childrenNames);
                        break;
                    case DFTElementType::BE:
                    case DFTElementType::CONSTF:
                    case DFTElementType::CONSTS:
                    case DFTElementType::VOT:
                    case DFTElementType::PAND:
                    case DFTElementType::SPARE:
                    case DFTElementType::POR:
                    case DFTElementType::PDEP:
                    case DFTElementType::SEQ:
                    case DFTElementType::MUTEX:
                        // Other elements are not supported
                        assert(false);
                        break;
                    default:
                        assert(false);
                }
                
                // Add parent with the new child newParent and all its remaining children
                childrenNames.clear();
                childrenNames.push_back(newParentName);
                for (auto const& child : originalParent->children()) {
                    if (std::find(rewrites.begin()+1, rewrites.end(), child->id()) == rewrites.end()) {
                        // Child was not rewritten and must be kept
                        childrenNames.push_back(child->name());
                    }
                }
                builder.copyGate(originalParent, childrenNames);
            }
            
            builder.setTopLevel(mElements[mTopLevelIndex]->name());
            // TODO use reference?
            DFT<ValueType> newDft = builder.build();
            return newDft.optimize();
        }
        
        template<typename ValueType>
        std::string DFT<ValueType>::getElementsString() const {
            std::stringstream stream;
            for (auto const& elem : mElements) {
                stream << "[" << elem->id() << "]" << elem->toString() << std::endl;
            }
            return stream.str();
        }

        template<typename ValueType>
        std::string DFT<ValueType>::getInfoString() const {
            std::stringstream stream;
            stream << "Top level index: " << mTopLevelIndex << ", Nr BEs" << mNrOfBEs;
            return stream.str();
        }

        template<typename ValueType>
        std::string DFT<ValueType>::getSpareModulesString() const {
            std::stringstream stream;
            stream << "[" << mElements[mTopLevelIndex]->id() << "] {";
            std::vector<size_t>::const_iterator it = mTopModule.begin();
            if (it == mTopModule.end()) {
                stream << "}" << std::endl;
                return stream.str();
            }
            assert(it != mTopModule.end());
            stream << mElements[(*it)]->name();
            ++it;
            while(it != mTopModule.end()) {
                stream <<  ", " << mElements[(*it)]->name();
                ++it;
            }
            stream << "}" << std::endl;

            for(auto const& spareModule : mSpareModules) {
                stream << "[" << mElements[spareModule.first]->name() << "] = {";
                if (!spareModule.second.empty()) {
                    std::vector<size_t>::const_iterator it = spareModule.second.begin();
                    assert(it != spareModule.second.end());
                    stream << mElements[(*it)]->name();
                    ++it;
                    while(it != spareModule.second.end()) {
                        stream <<  ", " << mElements[(*it)]->name();
                        ++it;
                    }
                }
                stream << "}" << std::endl;
            }
            return stream.str();
        }

        template<typename ValueType>
        std::string DFT<ValueType>::getElementsWithStateString(DFTStatePointer const& state) const{
            std::stringstream stream;
            for (auto const& elem : mElements) {
                stream << "[" << elem->id() << "]";
                stream << elem->toString();
                if (elem->isDependency()) {
                    stream << "\t** " << storm::storage::toChar(state->getDependencyState(elem->id()));
                } else {
                    stream << "\t** " << storm::storage::toChar(state->getElementState(elem->id()));
                    if(elem->isSpareGate()) {
                        size_t useId = state->uses(elem->id());
                        if(useId == elem->id() || state->isActive(useId)) {
                            stream << "actively ";
                        }
                        stream << "using " << useId;
                    }
                }
                stream << std::endl;
            }
            return stream.str();
        }

        // TODO rewrite to only use bitvector and id
        template<typename ValueType>
        std::string DFT<ValueType>::getStateString(DFTStatePointer const& state) const{
            std::stringstream stream;
            stream << "(" << state->getId() << ") ";
            for (auto const& elem : mElements) {
                if (elem->isDependency()) {
                    stream << storm::storage::toChar(state->getDependencyState(elem->id())) << "[dep]";
                } else {
                    stream << storm::storage::toChar(state->getElementState(elem->id()));
                    if(elem->isSpareGate()) {
                        stream << "[";
                        size_t useId = state->uses(elem->id());
                        if(useId == elem->id() || state->isActive(useId)) {
                            stream << "actively ";
                        }
                        stream << "using " << useId << "]";
                    }
                }
            }
            return stream.str();
        }
        
        template<typename ValueType>
        size_t DFT<ValueType>::getChild(size_t spareId, size_t nrUsedChild) const {
            assert(mElements[spareId]->isSpareGate());
            return getGate(spareId)->children()[nrUsedChild]->id();
        }
        
        template<typename ValueType>
        size_t DFT<ValueType>::getNrChild(size_t spareId, size_t childId) const {
            assert(mElements[spareId]->isSpareGate());
            DFTElementVector children = getGate(spareId)->children();
            for (size_t nrChild = 0; nrChild < children.size(); ++nrChild) {
                if (children[nrChild]->id() == childId) {
                    return nrChild;
                }
            }
            assert(false);
        }
        
        template <typename ValueType>
        std::vector<size_t> DFT<ValueType>::getIndependentSubDftRoots(size_t index) const {
            auto elem = getElement(index);
            auto ISD = elem->independentSubDft(false);
            return ISD;
        }

        template<typename ValueType>
        std::vector<size_t> DFT<ValueType>::immediateFailureCauses(size_t index) const {
            if(isGate(index)) {

            } else {
                return {index};
            }
        }

        template<typename ValueType>
        DFTColouring<ValueType> DFT<ValueType>::colourDFT() const {
            return DFTColouring<ValueType>(*this);
        }


        template<typename ValueType>
        DFTIndependentSymmetries DFT<ValueType>::findSymmetries(DFTColouring<ValueType> const& colouring) const {
            std::vector<size_t> vec;
            vec.reserve(nrElements());
            storm::utility::iota_n(std::back_inserter(vec), nrElements(), 0);
            BijectionCandidates<ValueType> completeCategories = colouring.colourSubdft(vec);
            std::map<size_t, std::vector<std::vector<size_t>>> res;
            
            for(auto const& colourClass : completeCategories.gateCandidates) {
                if(colourClass.second.size() > 1) {
                    std::set<size_t> foundEqClassFor;
                    for(auto it1 = colourClass.second.cbegin(); it1 != colourClass.second.cend(); ++it1) {
                        std::vector<std::vector<size_t>> symClass;
                        if(foundEqClassFor.count(*it1) > 0) {
                            // This item is already in a class.
                            continue;
                        }
                        if(!getGate(*it1)->hasOnlyStaticParents()) {
                            continue;
                        }
                        
                        std::pair<std::vector<size_t>, std::vector<size_t>> influencedElem1Ids = getSortedParentAndOutDepIds(*it1);
                        auto it2 = it1;
                        for(++it2; it2 != colourClass.second.cend(); ++it2) {
                            if(!getGate(*it2)->hasOnlyStaticParents()) {
                                continue;
                            }
                            std::vector<size_t> sortedParent2Ids = getGate(*it2)->parentIds();
                            std::sort(sortedParent2Ids.begin(), sortedParent2Ids.end());
                            
                            if(influencedElem1Ids == getSortedParentAndOutDepIds(*it2)) {
                                std::cout << "Considering ids " << *it1 << ", " << *it2 << " for isomorphism." << std::endl;
                                bool isSymmetry = false;
                                std::vector<size_t> isubdft1 = getGate(*it1)->independentSubDft(false);
                                std::vector<size_t> isubdft2 = getGate(*it2)->independentSubDft(false);
                                if(isubdft1.empty() || isubdft2.empty() || isubdft1.size() != isubdft2.size()) {
                                    continue;
                                }
                                std::cout << "Checking subdfts from " << *it1 << ", " << *it2 << " for isomorphism." << std::endl;
                                auto LHS = colouring.colourSubdft(isubdft1);
                                auto RHS = colouring.colourSubdft(isubdft2);
                                auto IsoCheck = DFTIsomorphismCheck<ValueType>(LHS, RHS, *this);
                                isSymmetry = IsoCheck.findIsomorphism();
                                if(isSymmetry) {
                                    std::cout << "subdfts are symmetric" << std::endl;
                                    foundEqClassFor.insert(*it2);
                                    if(symClass.empty()) {
                                        for(auto const& i : isubdft1) {
                                            symClass.push_back(std::vector<size_t>({i}));
                                        }
                                    }
                                    auto symClassIt = symClass.begin();
                                    for(auto const& i : isubdft1) {
                                        symClassIt->emplace_back(IsoCheck.getIsomorphism().at(i));
                                        ++symClassIt;
                                        
                                    }
                                    
                                }
                            }
                        }
                        if(!symClass.empty()) {
                            res.emplace(*it1, symClass);
                        }
                    }
                    
                }
            }
            return DFTIndependentSymmetries(res);
        }
        
        template<typename ValueType>
        std::vector<size_t> DFT<ValueType>::findModularisationRewrite() const {
           for(auto const& e : mElements) {
               if(e->isGate() && (e->type() == DFTElementType::AND || e->type() == DFTElementType::OR) ) {
                   // suitable parent gate! - Lets check the independent submodules of the children
                   auto const& children = std::static_pointer_cast<DFTGate<ValueType>>(e)->children();
                   for(auto const& child : children) {
                       //std::cout << "check idea for: " << child->id() << std::endl;;
                       auto ISD = std::static_pointer_cast<DFTGate<ValueType>>(child)->independentSubDft(true);
                       // In the ISD, check for other children:
                       //std::cout << "** subdft = ";
                       for(auto const& isdelemid : ISD) {
                           std::cout << isdelemid << " ";
                       }
                       std::cout << std::endl;
                       
                       std::vector<size_t> rewrite = {e->id(), child->id()};
                       for(size_t isdElemId : ISD) {
                           if(isdElemId == child->id()) continue;
                           if(std::find_if(children.begin(), children.end(), [&isdElemId](std::shared_ptr<DFTElement<ValueType>> const& e) { return e->id() == isdElemId; } ) != children.end()) {
                               //std::cout << "** found child in subdft: " <<  isdElemId << std::endl;
                               rewrite.push_back(isdElemId);
                           }
                       }
                       if(rewrite.size() > 2 && rewrite.size() < e->children().size() - 1) {
                           return rewrite;
                       }
                       
                   }    
               }
           } 
           return {};
        }
    

        template<typename ValueType>
        std::pair<std::vector<size_t>, std::vector<size_t>> DFT<ValueType>::getSortedParentAndOutDepIds(size_t index) const {
            std::pair<std::vector<size_t>, std::vector<size_t>> res;
            res.first = getElement(index)->parentIds();
            std::sort(res.first.begin(), res.first.end());
            for(auto const& dep : getElement(index)->outgoingDependencies()) {
                res.second.push_back(dep->id());
            }
            std::sort(res.second.begin(), res.second.end());
            return res;
        }
        
        // Explicitly instantiate the class.
        template class DFT<double>;

#ifdef STORM_HAVE_CARL
        template class DFT<RationalFunction>;
#endif

    }
}
