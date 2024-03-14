#include "query_node.h"

import stl;
import status;
import infinity_exception;
import logger;
import doc_iterator;
import term_doc_iterator;
import and_iterator;
import and_not_iterator;
import or_iterator;
import table_entry;
import column_index_reader;
import match_data;
import posting_iterator;

namespace infinity {

// optimize: from leaf to root, replace tree node in place

// expected property of optimized node:
// 1. children of "not" can only be term, "and" or "and_not", because "not" is not allowed, and "or" will be flattened to not list
// 2. children of "and" can only be term or "or", because "and", "not", "and_not" will be optimized
// 3. children of "or" can only be term, "and" or "and_not", because "or" will be optimized, and "not" is either optimized or not allowed
// 4. "and_not" does not exist in parser output, it is generated during optimization
//    "and_not": first child can be term, "and", "or", other children form a list of "not"

// 1. deal with "not": "not" must finally combine with parent "and" and turn into "and_not"
// properties of "not":
// 1.1. parent of "not" cannot be "not"
// 1.2. if parent is "and", then "not" should be combined with "and" and turn into "and_not"
// 1.3. if parent is "or", then the children of parent "or" must all be "not", and the "or" should be turned into "not (child and ...)"
// example:
// valid query: "A and (not B)" => "A and_not B"
// valid query: "A and ((not B) and (not C))" => "A and_not (B, C)"
// valid query: "A and ((not B) or (not C))" => "A and_not (B and C)"
// invalid query: "A or (not B)" : can't generate "and_not"
// invalid query: "A and ((not B) or C)" : subexpression "(not B) or C" is invalid
// here it is equivalent to "(A and_not B) or (A and C)", but it is more simple to disallow this case

std::unique_ptr<QueryNode> NotQueryNode::OptimizeInPlaceInner(std::unique_ptr<QueryNode> &) {
    if (children_.empty()) {
        UnrecoverableError("Invalid query statement: NotQueryNode node should have at least 1 child");
    }
    std::vector<std::unique_ptr<QueryNode>> new_not_list;
    for (auto &child : children_) {
        switch (child->GetType()) {
            case QueryNodeType::TERM:
            case QueryNodeType::AND:
            case QueryNodeType::AND_NOT: {
                new_not_list.emplace_back(std::move(child));
                break;
            }
            case QueryNodeType::OR: {
                auto &or_node = static_cast<OrQueryNode &>(*child);
                for (auto &or_child : or_node.children_) {
                    new_not_list.emplace_back(std::move(or_child));
                }
                break;
            }
            default: {
                UnrecoverableError("OptimizeInPlaceInner: Unexpected case!");
                break;
            }
        }
    }
    auto new_not_node = std::make_unique<NotQueryNode>(); // new node, weight is reset to 1.0
    new_not_node->children_ = std::move(new_not_list);
    return new_not_node;
}

// 2. deal with "and":
// rule for "and" (in execute order)
// 2.1. for all children,
//      if a child is "and" (it is already optimized, can only contain term or "or"), then flatten it into "and list"
//      if a child is term or "or", move it to "and list"
//      if a child is "not", move it to "not list", deal with it later
//      if a child is "and_not", move first child to "and list" (flatten it if it is an "and"), other children to "not list"
// 2.2. build result:
//      all cases:  "and list" | "not list"
//                       Y     |      Y       => build "and_not"
//                       Y     |      N       => build "and"
//                       N     |      Y       => build "not"

std::unique_ptr<QueryNode> AndQueryNode::OptimizeInPlaceInner(std::unique_ptr<QueryNode> &) {
    if (children_.size() < 2) {
        UnrecoverableError("Invalid query statement: AndQueryNode node should have at least 2 children");
    }
    std::vector<std::unique_ptr<QueryNode>> and_list;
    std::vector<std::unique_ptr<QueryNode>> not_list;
    // 2.1.
    for (auto &child : children_) {
        switch (child->GetType()) {
            case QueryNodeType::AND: {
                auto &and_node = static_cast<AndQueryNode &>(*child);
                and_list.insert(and_list.end(),
                                std::make_move_iterator(and_node.children_.begin()),
                                std::make_move_iterator(and_node.children_.end()));
                break;
            }
            case QueryNodeType::TERM:
            case QueryNodeType::OR: {
                and_list.emplace_back(std::move(child));
                break;
            }
            case QueryNodeType::NOT: {
                auto &not_node = static_cast<NotQueryNode &>(*child);
                not_list.insert(not_list.end(),
                                std::make_move_iterator(not_node.children_.begin()),
                                std::make_move_iterator(not_node.children_.end()));
                break;
            }
            case QueryNodeType::AND_NOT: {
                auto &and_not_node = static_cast<AndNotQueryNode &>(*child);
                if (auto &first_child = and_not_node.children_.front(); first_child->GetType() == QueryNodeType::AND) {
                    auto &and_node = static_cast<AndQueryNode &>(*first_child);
                    and_list.insert(and_list.end(),
                                    std::make_move_iterator(and_node.children_.begin()),
                                    std::make_move_iterator(and_node.children_.end()));
                } else {
                    and_list.emplace_back(std::move(first_child));
                }
                not_list.insert(not_list.end(),
                                std::make_move_iterator(std::next(and_not_node.children_.begin())),
                                std::make_move_iterator(and_not_node.children_.end()));
                break;
            }
            default: {
                UnrecoverableError("OptimizeInPlaceInner: Unexpected case!");
                break;
            }
        }
    }
    // 2.2.
    if (and_list.empty()) {
        // at least 2 children
        auto not_node = std::make_unique<NotQueryNode>(); // new node, weight is reset to 1.0
        not_node->children_ = std::move(not_list);
        return not_node;
    } else if (not_list.empty()) {
        // at least 2 children
        auto and_node = std::make_unique<AndQueryNode>(); // new node, weight is reset to 1.0
        and_node->children_ = std::move(and_list);
        return and_node;
    } else {
        auto and_not_node = std::make_unique<AndNotQueryNode>(); // new node, weight is reset to 1.0
        // build first child from and_list
        if (and_list.size() == 1) {
            and_not_node->children_.emplace_back(std::move(and_list.front()));
        } else {
            auto and_node = std::make_unique<AndQueryNode>(); // new node, weight is reset to 1.0
            and_node->children_ = std::move(and_list);
            and_not_node->children_.emplace_back(std::move(and_node));
        }
        // move not_list to and_not_node
        and_not_node->children_.insert(and_not_node->children_.end(),
                                       std::make_move_iterator(not_list.begin()),
                                       std::make_move_iterator(not_list.end()));
        return and_not_node;
    }
}

// 3. deal with "or":
// rule for "or" (in execute order)
// 3.1. for all children,
//      if a child is "or" (it is already optimized, can only contain term, "and" or "and_not"), flatten it into "or list"
//      if a child is term, "and" or "and_not", move it to "or list"
//      if a child is "not", move it to "not list", deal with it later
// 3.2. build result:
//      all cases:  "or list" | "not list"
//                       Y    |      Y       => invalid query
//                       Y    |      N       => build "or"
//                       N    |      Y       => build "not" of (child and ...)

std::unique_ptr<QueryNode> OrQueryNode::OptimizeInPlaceInner(std::unique_ptr<QueryNode> &) {
    if (children_.size() < 2) {
        UnrecoverableError("Invalid query statement: OrQueryNode node should have at least 2 children");
    }
    std::vector<std::unique_ptr<QueryNode>> or_list;
    std::vector<std::unique_ptr<QueryNode>> not_list;
    // 3.1.
    for (auto &child : children_) {
        switch (child->GetType()) {
            case QueryNodeType::OR: {
                auto &or_node = static_cast<OrQueryNode &>(*child);
                or_list.insert(or_list.end(), std::make_move_iterator(or_node.children_.begin()), std::make_move_iterator(or_node.children_.end()));
                break;
            }
            case QueryNodeType::TERM:
            case QueryNodeType::AND:
            case QueryNodeType::AND_NOT: {
                or_list.emplace_back(std::move(child));
                break;
            }
            case QueryNodeType::NOT: {
                not_list.emplace_back(std::move(child));
                break;
            }
            default: {
                UnrecoverableError("OptimizeInPlaceInner: Unexpected case!");
                break;
            }
        }
    }
    // 3.2.
    if (or_list.empty()) {
        // at least 2 children
        // build "not" of (child and ...)
        auto not_node = std::make_unique<NotQueryNode>(); // new node, weight is reset to 1.0
        auto and_node = std::make_unique<AndQueryNode>(); // new node, weight is reset to 1.0
        for (auto &not_child : not_list) {
            auto &not_child_node = static_cast<NotQueryNode &>(*not_child);
            if (not_child_node.children_.size() == 1) {
                and_node->children_.emplace_back(std::move(not_child_node.children_.front()));
            } else {
                // build "or" which exists in "not" children list
                auto or_node = std::make_unique<OrQueryNode>(); // new node, weight is reset to 1.0
                or_node->children_ = std::move(not_child_node.children_);
                and_node->children_.emplace_back(std::move(or_node));
            }
        }
        not_node->children_.emplace_back(std::move(and_node));
        return not_node;
    } else if (not_list.empty()) {
        // at least 2 children
        auto or_node = std::make_unique<OrQueryNode>(); // new node, weight is reset to 1.0
        or_node->children_ = std::move(or_list);
        return or_node;
    } else {
        UnrecoverableError("Invalid query statement: OrQueryNode node should not have both or list and not list");
        return nullptr;
    }
}

// 4. deal with "and_not":
// "and_not" does not exist in parser output, it is generated during optimization

std::unique_ptr<QueryNode> AndNotQueryNode::OptimizeInPlaceInner(std::unique_ptr<QueryNode> &) {
    UnrecoverableError("OptimizeInPlaceInner: Unexpected case! AndNotQueryNode should not exist in parser output");
    return nullptr;
}

// create search iterator

std::unique_ptr<DocIterator>
TermQueryNode::CreateSearch(const TableEntry *table_entry, IndexReader &index_reader, std::unique_ptr<Scorer> &scorer) const {
    ColumnID column_id = table_entry->GetColumnIdByName(column_);
    ColumnIndexReader *column_index_reader = index_reader.GetColumnIndexReader(column_id);
    if (!column_index_reader)
        return nullptr;
    PostingIterator *posting_iterator = column_index_reader->Lookup(term_, index_reader.session_pool_.get());
    if (posting_iterator == nullptr)
        return nullptr;
    auto search = MakeUnique<TermDocIterator>(posting_iterator, column_id);
    scorer->AddDocIterator(search.get(), column_id);
    return std::move(search);
}

std::unique_ptr<DocIterator>
AndQueryNode::CreateSearch(const TableEntry *table_entry, IndexReader &index_reader, std::unique_ptr<Scorer> &scorer) const {
    Vector<std::unique_ptr<DocIterator>> sub_doc_iters;
    sub_doc_iters.reserve(children_.size());
    for (auto &child : children_) {
        auto iter = child->CreateSearch(table_entry, index_reader, scorer);
        if (iter) {
            sub_doc_iters.emplace_back(std::move(iter));
        }
    }
    if (sub_doc_iters.empty()) {
        return nullptr;
    } else if (sub_doc_iters.size() == 1) {
        return std::move(sub_doc_iters[0]);
    } else {
        return MakeUnique<AndIterator>(std::move(sub_doc_iters));
    }
}

std::unique_ptr<DocIterator>
AndNotQueryNode::CreateSearch(const TableEntry *table_entry, IndexReader &index_reader, std::unique_ptr<Scorer> &scorer) const {
    Vector<std::unique_ptr<DocIterator>> sub_doc_iters;
    sub_doc_iters.reserve(children_.size());
    // check if the first child is a valid query
    auto first_iter = children_.front()->CreateSearch(table_entry, index_reader, scorer);
    if (!first_iter) {
        // no need to continue if the first child is invalid
        return nullptr;
    }
    sub_doc_iters.emplace_back(std::move(first_iter));
    for (u32 i = 1; i < children_.size(); ++i) {
        auto iter = children_[i]->CreateSearch(table_entry, index_reader, scorer);
        if (iter) {
            sub_doc_iters.emplace_back(std::move(iter));
        }
    }
    if (sub_doc_iters.size() == 1) {
        return std::move(sub_doc_iters[0]);
    } else {
        return MakeUnique<AndNotIterator>(std::move(sub_doc_iters));
    }
}

std::unique_ptr<DocIterator>
OrQueryNode::CreateSearch(const TableEntry *table_entry, IndexReader &index_reader, std::unique_ptr<Scorer> &scorer) const {
    Vector<std::unique_ptr<DocIterator>> sub_doc_iters;
    sub_doc_iters.reserve(children_.size());
    for (auto &child : children_) {
        auto iter = child->CreateSearch(table_entry, index_reader, scorer);
        if (iter) {
            sub_doc_iters.emplace_back(std::move(iter));
        }
    }
    if (sub_doc_iters.empty()) {
        return nullptr;
    } else if (sub_doc_iters.size() == 1) {
        return std::move(sub_doc_iters[0]);
    } else {
        return MakeUnique<OrIterator>(std::move(sub_doc_iters));
    }
}

std::unique_ptr<DocIterator>
NotQueryNode::CreateSearch(const TableEntry *table_entry, IndexReader &index_reader, std::unique_ptr<Scorer> &scorer) const {
    UnrecoverableError("NOT query node should be optimized into AND_NOT query node");
    return nullptr;
}

// print tree

std::string QueryNodeTypeToString(QueryNodeType type) {
    switch (type) {
        case QueryNodeType::INVALID:
            return "INVALID";
        case QueryNodeType::TERM:
            return "TERM";
        case QueryNodeType::AND:
            return "AND";
        case QueryNodeType::AND_NOT:
            return "AND_NOT";
        case QueryNodeType::OR:
            return "OR";
        case QueryNodeType::NOT:
            return "NOT";
        case QueryNodeType::WAND:
            return "WAND";
        case QueryNodeType::PHRASE:
            return "PHRASE";
        case QueryNodeType::PREFIX_TERM:
            return "PREFIX_TERM";
        case QueryNodeType::SUFFIX_TERM:
            return "SUFFIX_TERM";
        case QueryNodeType::SUBSTRING_TERM:
            return "SUBSTRING_TERM";
    }
}

void TermQueryNode::PrintTree(std::ostream &os, const std::string &prefix, bool is_final) const {
    os << prefix;
    os << (is_final ? "└──" : "├──");
    os << QueryNodeTypeToString(type_);
    os << " (weight: " << weight_ << ")";
    os << " (column: " << column_ << ")";
    os << " (term: " << term_ << ")";
    os << '\n';
}

void MultiQueryNode::PrintTree(std::ostream &os, const std::string &prefix, bool is_final) const {
    os << prefix;
    os << (is_final ? "└──" : "├──");
    os << QueryNodeTypeToString(type_);
    os << " (weight: " << weight_ << ")";
    os << " (children count: " << children_.size() << ")";
    os << '\n';
    std::string next_prefix = prefix + (is_final ? "    " : "│   ");
    for (u32 i = 0; i + 1 < children_.size(); ++i) {
        children_[i]->PrintTree(os, next_prefix, false);
    }
    children_.back()->PrintTree(os, next_prefix, true);
}

} // namespace infinity
