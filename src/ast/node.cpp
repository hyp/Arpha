#include "../base/base.h"
#include "../base/bigint.h"
#include "../base/symbol.h"
#include "../compiler.h"
#include "node.h"
#include "declarations.h"
#include "visitor.h"
#include "../intrinsics/ast.h"
#include "../intrinsics/types.h"

TypeExpression* Node::_returnType() const {
	return intrinsics::types::Void;
}

// Integer literals
IntegerLiteral::IntegerLiteral(const BigInt& integer){
	this->integer = integer;
	_type = nullptr;
}
TypeExpression* IntegerLiteral::_returnType() const{
	if(_type) return _type;
	//TODO <0 integers
	if(integer <= intrinsics::types::int32->integer->max) return intrinsics::types::int32;
	else if(integer <= intrinsics::types::uint32->integer->max) return intrinsics::types::uint32;
	else if(integer <= intrinsics::types::int64->integer->max) return intrinsics::types::int64;
	else return intrinsics::types::uint64;
}
Node* IntegerLiteral::duplicate() const {
	auto dup = new IntegerLiteral(integer);
	dup->_type = _type;
	return dup;
};
bool IntegerLiteral::isConst() const {
	return true;
}

// Unit expression
TypeExpression* UnitExpression::_returnType() const {
	return intrinsics::types::Void;
}
Node* UnitExpression::duplicate() const {
	return new UnitExpression;
};

// Wildcard expression
TypeExpression* WildcardExpression::_returnType() const {
	return intrinsics::types::Void;//TODO???
}
Node* WildcardExpression::duplicate() const {
	return new WildcardExpression;
};

// Expression reference
ExpressionReference::ExpressionReference(Node* node) : expression(node){}
TypeExpression* ExpressionReference::_returnType() const {
	return intrinsics::types::Expression;
}
Node* ExpressionReference::duplicate() const {
	return new ExpressionReference(expression->duplicate());
};

//Scope reference
ImportedScopeReference::ImportedScopeReference(ImportedScope* scope){
	this->scope = scope;
}
TypeExpression* ImportedScopeReference::_returnType() const {
	return intrinsics::types::Unresolved;
}
Node* ImportedScopeReference::duplicate() const {
	return scope->reference();
}

// Variable reference
VariableReference::VariableReference(Variable* variable){
	this->variable = variable;
}
bool VariableReference::isResolved() const {
	return variable->isResolved();
}
bool VariableReference::isLocal() const {
	return variable->isLocal();
}
TypeExpression* VariableReference::_returnType() const {
	return variable->type.type();
}
Node* VariableReference::duplicate() const {
	return new VariableReference(variable);
}

// Tuple expression
TupleExpression::TupleExpression() : type(nullptr) {}
TupleExpression::TupleExpression(Node* a,Node* b) : type(nullptr) {
	if( auto aIsTuple = a->asTupleExpression() ){
		children = aIsTuple->children;	
		delete a;
	}
	else children.push_back(a);
	children.push_back(b);
}

TypeExpression* TupleExpression::_returnType() const {
	assert(type);
	return type;
}
bool TupleExpression::isResolved() const {
	return type != nullptr;
}
Node* TupleExpression::duplicate() const {
	auto dup = new TupleExpression;
	for(auto i = children.begin();i!=children.end();i++){
		dup->children.push_back((*i)->duplicate());
	}
	dup->type = type->duplicate()->asTypeExpression();
	return dup;
};

// Assignment expression
AssignmentExpression::AssignmentExpression(Node* object,Node* value){
	this->object = object;
	this->value = value;
	isInitializingAssignment = false;
}
TypeExpression* AssignmentExpression::_returnType() const {
	return object->_returnType();
}
bool AssignmentExpression::isResolved() const {
	return false;//TODO
}
Node* AssignmentExpression::duplicate() const {
	auto e = new AssignmentExpression(object->duplicate(),value->duplicate());
	e->isInitializingAssignment = isInitializingAssignment;
	return e;
}

// Return expression
ReturnExpression::ReturnExpression(Node* expression) : value(expression) {}
Node* ReturnExpression::duplicate() const {
	return new ReturnExpression(value?value->duplicate():nullptr);
}

//Pointer operation
PointerOperation::PointerOperation(Node* expression,int type){
	this->expression = expression;
	kind = type;
}
TypeExpression* PointerOperation::_returnType() const {
	assert(expression->isResolved());
	auto next = expression->_returnType();//TODO
	if(next->type == TypeExpression::INTRINSIC) return intrinsics::types::Unresolved;
	if(kind == ADDRESS){
		auto x = new TypeExpression((PointerType*)nullptr,next);//TODO localPointer of local vars
		x->_localSemantics = expression->isLocal();
		return x;
	}
	else if(kind == DEREFERENCE && next->type == TypeExpression::POINTER){
		return next->argument;
	}
	return intrinsics::types::Unresolved;
}
bool PointerOperation::isResolved() const {
	return false;//TODO
}
Node* PointerOperation::duplicate() const {
	return new PointerOperation(expression->duplicate(),kind);
}

// Match expression
MatchExpression::MatchExpression(Node* object){
	this->object = object;
}
TypeExpression* MatchExpression::_returnType() const {
	return intrinsics::types::Void;//TODO
}
bool MatchExpression::isResolved() const {
	return false;
}
Node* MatchExpression::duplicate() const{
	auto dup = new MatchExpression(object);
	dup->cases.reserve(cases.size());
	for(auto i = cases.begin();i!=cases.end();i++){
		dup->cases.push_back(Case((*i).pattern->duplicate(),(*i).consequence ? (*i).consequence->duplicate() : nullptr,(*i).fallThrough));
	}
	return dup;
}

// Function reference
FunctionReference::FunctionReference(Function* func) : function(func) {
}
TypeExpression* FunctionReference::_returnType() const {
	assert(isResolved());
	return new TypeExpression(function->argumentType(),function->returnType());
}
bool FunctionReference::isResolved() const {
	return function->isResolved();//TODO kinds pointless, since function refrences are only obtained from resolved functions?
}
Node* FunctionReference::duplicate() const {
	return new FunctionReference(function);
}

// Field access expression
FieldAccessExpression::FieldAccessExpression(Node* object,int field){
	this->object = object;
	this->field = field;
	assert(objectsRecord());
}
Record* FieldAccessExpression::objectsRecord() const {
	auto type = object->_returnType();
	if(type->type == TypeExpression::RECORD) return type->record;
	else if(type->type == TypeExpression::POINTER && type->argument->type == TypeExpression::RECORD) return type->argument->record;
	return nullptr;
}
TypeExpression* FieldAccessExpression::_returnType() const {
	return objectsRecord()->fields[field].type.type();
}
Node* FieldAccessExpression::duplicate() const {
	return new FieldAccessExpression(object->duplicate(),field);
}

// Call expression

CallExpression::CallExpression(Node* object,Node* argument){
	this->object = object;
	this->arg = argument;
}

TypeExpression* CallExpression::_returnType() const {
	assert(isResolved());
	if( auto refFunc = object->asFunctionReference()){
		return refFunc->function->_returnType.type();
	}
	return intrinsics::types::Void;
}
bool CallExpression::isResolved() const {
	return false;
}
Node* CallExpression::duplicate() const {
	return new CallExpression(object->duplicate(),arg->duplicate());
}

//While expression
WhileExpression::WhileExpression(Node* condition,Node* body){
	this->condition = condition;
	this->body = body;
}
Node* WhileExpression::duplicate() const{
	return new WhileExpression(condition->duplicate(),body->duplicate());
}

// Block expression
BlockExpression::BlockExpression(Scope* scope){
	this->scope = scope;
}
Node* BlockExpression::duplicate() const {
	auto dup = new BlockExpression(scope);//scope->dup???
	dup->children.reserve(children.size());//Single alocation
	for(auto i=children.begin();i!=children.end();i++) dup->children.push_back((*i)->duplicate());
	return dup;
}
bool BlockExpression::isResolved() const {
	return false;
}

//Injects visitor callback and dynamic cast function into a node structure
//Note: only does the definitions, the appropriate implementations are done by traversing NODE_LIST
#define DECLARE_NODE_IMPLEMENTATION(T) \
	Node* T::accept(NodeVisitor* visitor) { \
		return visitor->visit(this);        \
	}										\
	T* T::as##T() { return this; }                        

NODE_LIST(DECLARE_NODE_IMPLEMENTATION)

#undef DECLARE_NODE_IMPLEMENTATION


//

TypeExpression* InferredUnresolvedTypeExpression::type(){
	return kind == Type ? _type : intrinsics::types::Unresolved;
}
void InferredUnresolvedTypeExpression::infer(TypeExpression* type){
	assert(kind == Inferred);
	assert(type->isResolved());
	kind = Type;
	_type = type->duplicate()->asTypeExpression();
}

// TypeExpression
TypeExpression::TypeExpression(IntrinsicType* intrinsic) : type(INTRINSIC),_localSemantics(false) {
	this->intrinsic = intrinsic;
}
TypeExpression::TypeExpression(IntegerType* integer) : type(INTEGER),_localSemantics(false) {
	this->integer = integer;
}
TypeExpression::TypeExpression(Record* record): type(RECORD),_localSemantics(false) { 
	this->record = record; 
}
TypeExpression::TypeExpression(PointerType* pointer,TypeExpression* next) : type(POINTER),_localSemantics(false) {
	this->argument = next;
}
TypeExpression::TypeExpression(TypeExpression* argument,TypeExpression* returns) : type(FUNCTION),_localSemantics(false) {
	this->argument = argument;
	this->returns = returns;
}
bool TypeExpression::isResolved() const {
	switch(type){
		case RECORD: return record->isResolved();
		case POINTER: return argument->isResolved();
		case FUNCTION: return argument->isResolved() && returns->isResolved();
	}
	return true;
}
TypeExpression* TypeExpression::_returnType() const {
	return isResolved() ? intrinsics::types::Type : intrinsics::types::Unresolved ;
}
Node* TypeExpression::duplicate() const {
	TypeExpression* x;
	switch(type){
		case RECORD: return record->reference();
		case INTEGER: return integer->reference();
		case INTRINSIC: return intrinsic->reference();
		case POINTER:
			x = new TypeExpression((PointerType*)nullptr,argument->duplicate()->asTypeExpression());
			x->_localSemantics = _localSemantics;
			return x;
		case FUNCTION:
			x = new TypeExpression(argument->duplicate()->asTypeExpression(),returns->duplicate()->asTypeExpression());
			x->_localSemantics = _localSemantics;
			return x;
		default:
			throw std::runtime_error("TypeExpression type invariant failed");
			return nullptr;
	}
}


size_t TypeExpression::size() const {
	switch(type){
		case RECORD: return record->size();
		case INTEGER: return integer->size();
		case INTRINSIC: return intrinsic->size();
		case POINTER: return compiler::pointerSize;
		case FUNCTION: return compiler::pointerSize;
		default:
			throw std::runtime_error("TypeExpression type invariant failed");
	}
}
bool TypeExpression::isSame(TypeExpression* other){
	if(this->type != other->type) return false;
	if(this->_localSemantics != other->_localSemantics) return false;
	switch(type){
		case RECORD: return record == other->record;
		case INTEGER: return integer == other->integer;
		case POINTER: return argument->isSame(other->argument);
		case INTRINSIC: return intrinsic == other->intrinsic;
		case FUNCTION: return argument->isSame(other->argument) && returns->isSame(other->returns);
		default:
			throw std::runtime_error("TypeExpression type invariant failed");	
			return false;
	}
}
Node* TypeExpression::assignableFrom(Node* expression,TypeExpression* type) {
	if(this->isSame(type)) return expression;//like a baws

	else if(this->type == INTEGER && type->type == INTEGER){
		//literal integer constants.. check to see if the type can accept it's value
		if(auto intConst = expression->asIntegerLiteral()){
			if(!intConst->_type && this->integer->isValid(intConst->integer)) return expression;
		}
	}else if(type->type == RECORD){
		//Extenders fields
		for(size_t i = 0;i < type->record->fields.size();i++){
			Record::Field* field = &type->record->fields[i];
			if(field->isExtending && field->type.isResolved()){
				auto dummyFieldAcess = new FieldAccessExpression(expression,i);
				if(auto assigns = this->assignableFrom(dummyFieldAcess,field->type.type())) return assigns;
				delete dummyFieldAcess;
			}
		}
	}else if(this->type == POINTER){
		if(type->type == POINTER){
			//TODO local semantics interaction
			
			//Extender records on pointers to records
			if(type->argument->type == RECORD){
				auto dummyDeref = new PointerOperation(expression,PointerOperation::DEREFERENCE);
				if(auto assigns = this->argument->assignableFrom(dummyDeref,type->argument)){
					debug("YES for pointer exetnder records!");
					return new PointerOperation(assigns,PointerOperation::ADDRESS);
				}
				delete dummyDeref;
			}
		}
	}

	return nullptr;
}
std::ostream& operator<< (std::ostream& stream,TypeExpression* node){
	if(node->hasLocalSemantics()) stream<<"local ";
	switch(node->type){
		case TypeExpression::RECORD: stream<<node->record; break;
		case TypeExpression::INTEGER: stream<<node->integer->id; break;
		case TypeExpression::INTRINSIC: stream<<node->intrinsic->id; break;
		case TypeExpression::POINTER: 
			stream<<"Pointer("<<node->argument<<')'; break;
		case TypeExpression::FUNCTION: 
			stream<<"FuncType("<<node->argument<<','<<node->returns<<')'; break;
	}
	return stream;
}

//Other,temporary nodes

ExpressionVerifier::ExpressionVerifier(const Location& loc,Node* child,TypeExpression* typeExpected) : expression(child),expectedType(typeExpected) {
	assert(child);assert(typeExpected);
	location = loc;
}
Node* ExpressionVerifier::duplicate() const {
	return new ExpressionVerifier(location,expression->duplicate(),expectedType/* NB no duplicate */);
}
std::ostream& operator<< (std::ostream& stream,ExpressionVerifier* node){
	stream<<node->expression<<" with expected type "<<node->expectedType;
	return stream;
}

UnresolvedSymbol::UnresolvedSymbol(const Location& loc,SymbolID sym,Scope* scope) : symbol(sym),explicitLookupScope(scope) {
	assert(!sym.isNull());
	location = loc;
}
Node* UnresolvedSymbol::duplicate() const {
	return new UnresolvedSymbol(location,symbol,explicitLookupScope);
}
std::ostream& operator<< (std::ostream& stream,UnresolvedSymbol* node){
	stream<<node->symbol;
	return stream;
}

AccessExpression::AccessExpression(Node* object,SymbolID symbol){
	this->object = object;
	this->symbol = symbol;
	this->passedFirstEval = false;
}
Node* AccessExpression::duplicate() const {
	return new AccessExpression(object->duplicate(),symbol);//NB duplicate passed first eval? - no
}
std::ostream& operator<< (std::ostream& stream,AccessExpression* node){
	stream<<node->object<<" u. "<<node->symbol;
	return stream;
}

ErrorExpression* errorInstance = nullptr;
Node* ErrorExpression::duplicate() const {
	return errorInstance;//NB an instance is already created, so we dont have to use getInstance
};
ErrorExpression* ErrorExpression::getInstance() {
	if(errorInstance) return errorInstance;
	else return errorInstance = new ErrorExpression;
}

/**
* Node tracer
*/

struct NodeToString: NodeVisitor {
	std::ostream& stream;
	NodeToString(std::ostream& ostream) : stream(ostream) {}

	Node* visit(UnresolvedSymbol* node){
		stream<<node;
		return node;
	}
	Node* visit(ExpressionVerifier* node){
		stream<<node;
		return node;
	}
	Node* visit(AccessExpression* node){
		stream<<node;
		return node;
	}
	Node* visit(IntegerLiteral* node){
		stream<<node->integer;
		return node;
	}
	Node* visit(ErrorExpression* node){
		stream<<"error";
		return node;
	}
	Node* visit(UnitExpression* node){
		stream<<"()";
		return node;
	}
	Node* visit(WildcardExpression* node){
		stream<<"_";
		return node;
	}
	Node* visit(ExpressionReference* node){
		stream<<"eref "<<node->expression;
		return node;
	}
	Node* visit(ImportedScopeReference* node){
		stream<<"scope-ref "<<node->scope->id;
		return node;
	}
	Node* visit(VariableReference* node){
		stream<<(node->variable->isLocal()?"local ":"")<<"variable "<<node->variable->id;
		return node;
	}
	Node* visit(FunctionReference* node){
		stream<<"ref func "<<node->function->id;
		return node;
	}

	Node* visit(CallExpression* node){
		stream<<"call "<<node->object<<" with "<<node->arg;
		return node;
	}
	Node* visit(FieldAccessExpression* node){
		stream<<node->object<<" f. "<<node->objectsRecord()->fields[node->field].name;
		return node;
	}

	Node* visit(AssignmentExpression* node){
		stream<<node->object<<" = "<<node->value;
		return node;
	}
	Node* visit(ReturnExpression* node){
		stream<<"return";
		if(node->value) stream<<' '<<node->value;
		return node;
	}
	Node* visit(PointerOperation* node){
		stream<<(node->kind==PointerOperation::ADDRESS?'&':'*')<<node->expression;
		return node;
	}
	Node* visit(MatchExpression* node){
		stream<<"match "<<node->object;
		for(auto i=node->cases.begin();i!=node->cases.end();i++){
			stream<<'|'<<(*i).pattern<<" => ";
			if((*i).consequence) stream<<(*i).consequence<<' ';
			if((*i).fallThrough) stream<<"fallthrough";
		}
		return node;
	}
	Node* visit(TupleExpression* node){
		auto i = node->children.begin();
		while(1){
			stream<<(*i);
			++i;
			if( i == node->children.end() ) break;
			stream<<',';
		}
		return node;
	}
	Node* visit(BlockExpression* node){
		stream<<"{\n  ";
		for(auto i =node->children.begin();i != node->children.end(); ++i) stream<<(*i)<<";\n  "; 
		stream<<"}";
		return node;
	}
	Node* visit(WhileExpression* node){
		stream<<"while "<<node->condition<<" do "<<node->body;
		return node;
	}
	Node* visit(TypeExpression* node){
		stream<<node;
		return node;
	}
};
std::ostream& operator<< (std::ostream& stream,Node* node){
	stream<<'(';
	if(!node->label().isNull()) stream<<node->label()<<':';
	NodeToString visitor(stream);
	node->accept(&visitor);
	stream<<')';
	stream<<"::";
	if(node->isResolved()) stream<<node->_returnType();
	else stream<<"unresolved";
	return stream;
}
