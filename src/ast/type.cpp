#include "../base/base.h"
#include "../base/bigint.h"
#include "../base/symbol.h"
#include "../compiler.h"
#include "node.h"
#include "declarations.h"
#include "visitor.h"
#include "interpret.h"
#include "../intrinsics/types.h"

data::gen::AbstractTarget::TypeSystemState Type::typeSystemState;

Type* TypePatternUnresolvedExpression::type() const {
	assert(kind == TYPE);
	return  _type;
}
TypePatternUnresolvedExpression TypePatternUnresolvedExpression::duplicate(DuplicationModifiers* mods) const {
	TypePatternUnresolvedExpression result;
	if(kind == TYPE) result._type = _type;
	else if(kind == UNRESOLVED) result.unresolvedExpression = unresolvedExpression->duplicate(mods);
	else if(kind == PATTERN) result.pattern = pattern ? pattern->duplicate(mods) : nullptr;
	result.kind = kind;
	return result;
}
void TypePatternUnresolvedExpression::specify(Type* givenType){
	assert(kind == PATTERN && pattern == nullptr);
	assert(givenType->isResolved());
	kind = TYPE;
	_type = givenType;
}
bool TypePatternUnresolvedExpression::deduce(Type* givenType,Scope* container){
	assert(isPattern());
	assert(givenType->isResolved());

	if(pattern){
		PatternMatcher matcher(container);
		if(!matcher.match(givenType,pattern)) return false;
	}
	kind  = TYPE;
	_type = givenType;
	return true;
}
TypePatternUnresolvedExpression::PatternMatcher::IntroducedDefinition* TypePatternUnresolvedExpression::PatternMatcher::lookupDefinition(SymbolID name){
	for(auto i = introducedDefinitions.begin();i!=introducedDefinitions.end();i++){
		if(name == (*i).name) return &(*i);
	}
	return nullptr;
}
void TypePatternUnresolvedExpression::PatternMatcher::introduceDefinition(SymbolID name,Location location,Node* value){
	for(auto i = introducedDefinitions.begin();i!=introducedDefinitions.end();i++){
		if(name == (*i).name) error(location,"Multiple type labels with same name %s exist in this type pattern",name);
	}
	introducedDefinitions.push_back(IntroducedDefinition(name,location,value));
}
bool TypePatternUnresolvedExpression::PatternMatcher::check(Node* expression){
	if(expression->asTypeReference()){ //| int32
		return true;
	}else if(auto unresolved = expression->asUnresolvedSymbol()){
		auto symbol = unresolved->symbol;
		if(symbol == "_"){
			if(!unresolved->label().isNull()) introduceDefinition(unresolved->label(),unresolved->location());
			return true; //|_ | T:_
		}
		//T
		if(lookupDefinition(symbol)) return true;
	} else if(auto ref = expression->asFunctionReference()){
		//Constraint
		if(ref->function->isFlagSet(Function::CONSTRAINT_FUNCTION) && ref->function->isResolved()){
			if(!ref->label().isNull()) introduceDefinition(ref->label(),ref->location());
			return true;
		}
	} else if(auto call = expression->asCallExpression()){
		//| Pointer(_) i.e. a Type generated by a function
		bool resolvedObject = false;
		if(auto callingUnresolvedFunction = call->object->asUnresolvedSymbol()){
			auto def = container->lookupPrefix(callingUnresolvedFunction->symbol);
			Overloadset* os = def ? def->asOverloadset() : nullptr;
			if(os && os->isFlagSet(Overloadset::TYPE_GENERATOR_SET)) resolvedObject = true;//TODO deep search?
		} else if(auto ref = call->object->asFunctionReference()){
			if(ref->function->isFlagSet(Function::CONSTRAINT_FUNCTION) && ref->function->isResolved()) resolvedObject = true;
		}
		if(resolvedObject){
			if(!call->label().isNull()) introduceDefinition(call->label(),call->location());
			//Check the parameters..
			if(auto argTuple = call->arg->asTupleExpression()){
				for(auto i = argTuple->children.begin();i!=argTuple->children.end();i++){ if(!check(*i)) return false; }
				return true;
			}
			else return check(call->arg);
		}
	} else if(auto var = expression->asVariableReference()){
		if(lookupDefinition(var->variable->label())) return true;
	}
	return false;
}
//Evaluates the verifier to see if an expression satisfies a constraint
bool satisfiesConstraint(Node* arg,Function* constraint,Scope* expansionScope){
	assert(constraint->arguments.size() == 1);
	
	CTFEinvocation i(compiler::currentUnit(),constraint);
	if(i.invoke(arg)){
		if(auto resolved = i.result()->asBoolExpression()){
			return resolved->value;
		}
	}
	error(arg,"Can't evaluate constraint %s with argument %s at compile time:\n\tCan't evaluate expression %s!",constraint->label(),arg,i.result());
	return false;
}
bool TypePatternUnresolvedExpression::PatternMatcher::match(Type* type,Node* pattern){
	auto ref = new TypeReference(type);
	return match(ref,pattern);
}
bool TypePatternUnresolvedExpression::PatternMatcher::match(Node* object,Node* pattern){
	auto typeRef = object->asTypeReference();
	auto type = typeRef ? typeRef->type : nullptr;
	//_ | T
	if(auto unresolved = pattern->asUnresolvedSymbol()){
		auto symbol = unresolved->symbol;
		if(symbol == "_"){
			if(!unresolved->label().isNull()) introduceDefinition(unresolved->label(),unresolved->location(),object);
			return true; //|_ | T:_
		}
		//T
		if(auto def = lookupDefinition(symbol)){
			if(auto vt = def->value->asTypeReference())
				return type && type->isSame(vt->type);
			else return false;
		}
	} else if(auto var = pattern->asVariableReference()){ //shadowed T
		if(auto def = lookupDefinition(var->variable->label())){
			if(auto vt = def->value->asTypeReference())
					return type && type->isSame(vt->type);
				else return false;
		}
	}

	//Match(non-type) TODO
	if(!type){
		//match non type
		assert(false);
		return false;
	}
	//Match(type)
	if(auto type2 = pattern->asTypeReference()) return type->isSame(type2->type); //| int32
	else if(auto ref = pattern->asFunctionReference()){
		//Constraint (We can safely assume this is a constraint if check passed)
		if(satisfiesConstraint(typeRef,ref->function,container)){
			if(!ref->label().isNull()) introduceDefinition(ref->label(),ref->location(),typeRef);
			return true;
		}
	} else if(auto call = pattern->asCallExpression()){
		if(!type->wasGenerated()) return false;
		bool matchedObject = false;
		//| Pointer(_)
		if(auto callingUnresolvedFunction = call->object->asUnresolvedSymbol()){
			auto def = container->lookupPrefix(callingUnresolvedFunction->symbol);
			Overloadset* os = def ? def->asOverloadset() : nullptr;
			if(os && os->isFlagSet(Overloadset::TYPE_GENERATOR_SET)){//TODO better search?
				for(auto i = os->functions.begin();i!=os->functions.end();i++){
					if(type->wasGeneratedBy(*i)) matchedObject = true;
				}
			}
		} else if(auto ref = call->object->asFunctionReference()){
			//Constraint (We can safely assume this is a constraint if check passed)
			if(satisfiesConstraint(typeRef,ref->function,container)) matchedObject = true;
		}
		if(matchedObject){
			//Match parameters..
			if(!call->label().isNull()) introduceDefinition(call->label(),call->location(),typeRef);
			if(auto argTuple = call->arg->asTupleExpression()){
				size_t j = 0;
				for(auto i = argTuple->children.begin();i!=argTuple->children.end();i++,j++){ if(!match(type->generatedArgument(j),*i)) return false; }
				return true;
			}
			else return match(type->generatedArgument(0),call->arg);
		}
	} 
	return false;
}

void TypePatternUnresolvedExpression::PatternMatcher::defineIntroducedDefinitions(){
	//TODO check if the scope already contains them..
	for(auto i= introducedDefinitions.begin();i!=introducedDefinitions.end();i++){
		auto var = new Variable((*i).name,(*i).location);
		var->_owner = container;
		var->isMutable = false;
		if((*i).value){
			var->specifyType((*i).value->returnType());
			var->setImmutableValue((*i).value);
		}else{
			//TODO def f(x T:_) = T define T with no value
		}
		container->define(var);
	}
}

/**
* The type
*/
Type::Type(int kind) : type(kind),flags(0) {
	assert(kind == VOID || kind == TYPE || kind == BOOL || kind == RECORD || kind == VARIANT);
}
Type::Type(IntegerType* integer) : type(INTEGER),flags(0) {
	this->integer = integer;
}
Type::Type(Record* record) : type(RECORD),flags(0) {
	this->record = record;
}
Type::Type(int kind,Type* next) : type(kind),flags(0) {
	assert(kind == POINTER || kind == POINTER_BOUNDED);
	this->argument = next;
}
Type::Type(Type* argument,Type* returns) : type(FUNCTION),flags(0) {
	this->argument = argument;
	this->returns = returns;
}
Type::Type(int kind,Type* T,size_t N) : type(kind),flags(0) {
	assert(kind == STATIC_ARRAY || kind == POINTER_BOUNDED_CONSTANT);
	argument = T;
	this->N  = N;
}
Type::Type(int kind,int subtype) : type(NODE),flags(0) {
	assert(kind == NODE);
	nodeSubtype = subtype;
}
void Type::setFlag(uint16 flag){
	flags |= flag;
}
bool Type::isFlagSet(uint16 flag) const {
	return (flags & flag) == flag;
}
bool Type::isValidTypeForVariable(){
	if(type == NODE) return false;
	return true;
}
bool Type::isValidTypeForArgument(){
	if(type == NODE) return false;
	return true;
}
bool Type::isResolved() const {
	switch(type){
		case RECORD:  return record->isResolved();
		case POINTER:
		case POINTER_BOUNDED:
		case POINTER_BOUNDED_CONSTANT:
		case STATIC_ARRAY:
			return argument->isResolved();
		case FUNCTION:
			return argument->isResolved() && returns->isResolved();
		default:
			return true;
	}
}
//Partially resolved means sizeof(T) would succeed
bool Type::isPartiallyResolved() const {
	switch(type){
	case RECORD:  return record->isResolved();
	default:
		return true;
	}
}
bool Type::matchRecord(Record* record) const {
	return type == RECORD && this->record == record;
}

//TODO non references
size_t Type::size() const {
	switch(type){
		case VOID: case TYPE: return 0;
		case BOOL: return 1;
		case RECORD: return record->size();
		case INTEGER: return integer->size();
		case POINTER: 
		case POINTER_BOUNDED_CONSTANT:
			return typeSystemState.pointerSizeof;
		case FUNCTION:
			return typeSystemState.functionPointerSizeof;
		case POINTER_BOUNDED:
			return typeSystemState.pointerSizeof*2;
		case STATIC_ARRAY: return argument->size() * N;
		default:
			throw std::runtime_error("TypeExpression type invariant failed");
	}
}
uint32 Type::alignment() const {
	return size(); //TODO
}
bool Type::isSame(Type* other){
	if(this->type != other->type) return false;
	switch(type){
		case VOID: case TYPE: case BOOL: return type == other->type;
		case RECORD: return record == other->record;
		case INTEGER: return integer == other->integer;
		case POINTER: return argument->isSame(other->argument);
		case FUNCTION: return argument->isSame(other->argument) && returns->isSame(other->returns);
		case POINTER_BOUNDED: return argument->isSame(other->argument);
		case POINTER_BOUNDED_CONSTANT: return argument->isSame(other->argument) && N == other->N;
		case STATIC_ARRAY: return argument->isSame(other->argument) && N == other->N;
		case NODE: return nodeSubtype == other->nodeSubtype;
		default:
			throw std::runtime_error("TypeExpression type invariant failed");	
			return false;
	}
}
bool Type::wasGenerated() const {
	switch(type){
	case POINTER: return true;
	case FUNCTION: return true;
	case RECORD: return record->wasGenerated();
	case POINTER_BOUNDED: return true;
	case POINTER_BOUNDED_CONSTANT: return true;
	case STATIC_ARRAY: return true;
	default: return false;
	}
}
bool Type::wasGeneratedBy(Function* function) const {
	switch(type){
	case POINTER: return function == intrinsics::types::PointerTypeGenerator;
	case FUNCTION: return function == intrinsics::types::FunctionTypeGenerator;
	case RECORD: return record->wasGeneratedBy(function);
	case STATIC_ARRAY: return function == intrinsics::types::StaticArrayTypeGenerator;
	case POINTER_BOUNDED: return function == intrinsics::types::BoundedPointerTypeGenerator;
	case POINTER_BOUNDED_CONSTANT: return function == intrinsics::types::BoundedConstantLengthPointerTypeGenerator;
	default:
		return false;
	}
}
Node* Type::generatedArgument(size_t i) const {
	switch(type){
	case POINTER:
	case POINTER_BOUNDED:
		return new TypeReference(argument);
	case FUNCTION: return new TypeReference(i == 0 ? argument : returns);
	case RECORD: return record->generatedArgument(i);
	case STATIC_ARRAY: 
	case POINTER_BOUNDED_CONSTANT:
		return i == 0 ? new TypeReference(argument) : (Node*) new IntegerLiteral((uint64)N);
	default:
		throw std::runtime_error("TypeExpression generatedArgument failed");	
		return nullptr;
	}
}

enum {
	LITERAL_CONVERSION = 4,
	RECORD_SUBTYPE,
	EXACT
};
int   Type::canAssignFrom(Node* expression,Type* type){
	if(this->isSame(type)) return EXACT;

	else if(this->type == INTEGER && type->type == INTEGER){
		//literal integer constants.. check to see if the type can accept it's value
		if(auto intConst = expression->asIntegerLiteral()){
			//literal match
			if(!intConst->_type && this->integer->isValid(intConst->integer)) return LITERAL_CONVERSION;
		}
		if(type->integer->isSubset(this->integer)) return RECORD_SUBTYPE;
	}else if(type->type == RECORD){
		//Extenders fields
		for(size_t i = 0;i < type->record->fields.size();i++){
			Record::Field* field = &type->record->fields[i];
			if(field->isExtending && field->type.isResolved()){
				auto dummyFieldAcess = new FieldAccessExpression(expression,i);
				if(this->canAssignFrom(dummyFieldAcess,field->type.type()) != -1){
					delete dummyFieldAcess;
					return RECORD_SUBTYPE;
				}
				delete dummyFieldAcess;
			}
		}
	}
	else if(this->type == POINTER){
		if(type->type == POINTER){		
			//Extender records on pointers to records
			if(type->argument->type == RECORD){
				auto dummyDeref = new PointerOperation(expression,PointerOperation::DEREFERENCE);
				dummyDeref->setFlag(Node::RESOLVED);
				if(this->argument->canAssignFrom(dummyDeref,type->argument) != -1){
					delete dummyDeref;
					return RECORD_SUBTYPE;
				}
				delete dummyDeref;
			}
			//subnode upcasts to node
			else if(next()->type == NODE && next()->nodeSubtype == -1 && type->next()->type == NODE) return RECORD_SUBTYPE;
		}
	}
	return -1;
}
Node* Type::assignableFrom(Node* expression,Type* type) {
	if(this->isSame(type)) return expression;//like a baws

	else if(this->type == INTEGER && type->type == INTEGER){
		//literal integer constants.. check to see if the type can accept it's value
		if(auto intConst = expression->asIntegerLiteral()){
			if(!intConst->_type && this->integer->isValid(intConst->integer)){
				intConst->_type = this->integer;
				return expression;
			}
		}
		if(type->integer->isSubset(this->integer)) return expression;
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
				dummyDeref->setFlag(Node::RESOLVED);
				if(auto assigns = this->argument->assignableFrom(dummyDeref,type->argument)){
					debug("YES for pointer exetnder records!");
					return new PointerOperation(assigns,PointerOperation::ADDRESS);
				}
				delete dummyDeref;
			}
			//subnode upcasts to node
			else if(next()->type == NODE && next()->nodeSubtype == -1 && type->next()->type == NODE) return expression;
		}
	} else if(this->type == POINTER_BOUNDED && type->type == POINTER_BOUNDED_CONSTANT && this->next() == type->next()){
		return new CastExpression(expression,this); // auto cast
	}

	return nullptr;
}

/**
* Aggregate type
*/
AggregateType::AggregateType(int typeKind,SymbolID name,Location& location) : PrefixDefinition(name,location),_type(typeKind) {
	_size = 0;
}
bool AggregateType::wasGeneratedBy(Function* func) const {
	return func == _owner->functionOwner()->generatedFunctionParent;
}
Node* AggregateType::generatedArgument(size_t i) const {
	return _owner->functionOwner()->expandedArguments[i];
}
Node*  AggregateType::createReference(){
	return new TypeReference(&_type);
}
size_t AggregateType::size(){
	assert(isResolved());
	return _size;
}

/**
* Record type
*/
Record::Record(SymbolID name,Location& location) : AggregateType(Type::RECORD,name,location) {
	headRecord   = nullptr;
	_type.record = this;
}
int Record::lookupField(const SymbolID fieldName) const {
	for(size_t i = 0;i<fields.size();i++){
		if( fields[i].name == fieldName ) return int(i);
	}
	return -1;
}
void Record::add(const Field& var){
	assert(!isResolved());
	fields.push_back(var);
}
void Record::calculateResolvedProperties(){
	//sizeof
	_size = 0;
	for(auto i = fields.begin();i!=fields.end();++i){
		_size += (*i).type.type()->size();
		if((*i).isExtending) setFlag(HAS_DERIVING_HIERARCHY);
	}
}
Record::Field Record::Field::duplicate(DuplicationModifiers* mods) const{
	Field result;
	result.name = name;
	result.type = type.duplicate(mods);
	result.isExtending = isExtending;
	return result;
}
Node* Record::duplicate(DuplicationModifiers* mods) const {
	if(headRecord) assert(false);

	auto rec = new Record(label(),location());
	mods->duplicateDefinition(const_cast<Record*>(this),rec);
	rec->_owner = mods->target;
	for(auto i = fields.begin();i!=fields.end();i++){
		rec->fields.push_back((*i).duplicate(mods));
	}
	rec->_size = _size;
	return copyProperties(rec);
}

std::ostream& operator<< (std::ostream& stream,Record* type){
	if(type->isAnonymous()){
		stream<<"anon-record"; 
		for(size_t i = 0;i < type->fields.size();i++){
			stream<<(i == 0 ? '(' : ',')<<(type->fields[i].name.isNull()?"_":type->fields[i].name)<<":"<<type->fields[i].type.type();
		}
		stream<<')';
	}
	else stream<<type->label();
	return stream;
}

/**
*Organized as
*  record(int32,int32) <-- headRecord
*     record(x: int32,y:int32) <-- subRecord
*     record(width: int32, height: int32)
*  record(int32,bool)
*/
std::vector<std::pair<Record*,std::vector<Record*> > > records;

Record* Record::createRecordType(std::vector<Field>& record,Record* headRecord){
	std::string typeName = "record";
	for(size_t i=0;i<record.size();i++)//TODO change this ludicrous display
		typeName+=format("%c%s:%s",i == 0 ? '(' : ',',record[i].name.isNull() ?  "_" : record[i].name.ptr(),record[i].type.type());
	typeName+=')';
	debug("Tuple created: %s",typeName);

	auto type=new Record(typeName.c_str(),Location(0,0));
	type->headRecord = headRecord ? headRecord : type;
	for(size_t i=0;i<record.size();i++){
		type->add(Field(headRecord!=nullptr?(record[i].name):(SymbolID()),record[i].type.type() ));
	}
	type->setFlag(Node::RESOLVED);
	type->calculateResolvedProperties();
	return type;
}

Record* Record::findSubRecord(Record* headRecord,std::vector<Record*>& subRecords,std::vector<Field>& record){
	//Match the names to the corresponding record
	for(auto i=subRecords.begin();i!=subRecords.end();i++){
		auto subRecord = *i;
		auto areFieldsSameName = true;
		for(size_t j=0;j < record.size();j++){
			if(subRecord->fields[j].name != record[j].name) areFieldsSameName = false;
		}
		if(areFieldsSameName) return subRecord;
	}
	//If the record doesn't exist we have to add it to subrecords
	auto t = createRecordType(record,headRecord);
	subRecords.push_back(t);
	return t;
}

Record* Record::findAnonymousRecord(std::vector<Field>& record){
	assert(record.size() > 1);

	//Check to see if the record has named fields
	auto areFieldsUnnamed = true;
	for(size_t j=0;j < record.size();j++){
		if(!record[j].name.isNull()) areFieldsUnnamed = false;
		assert(record[j].type.isResolved());
	}

	//Check to see if the following record already exists.
	//Match the types to the corresponding tuple
	for(auto i=records.begin();i!=records.end();i++){
		auto headRecord = (*i).first;
		if(headRecord->fields.size() == record.size()){
			auto areFieldsSameType = true;			
			for(size_t j=0;j < record.size();j++){
				if(!(headRecord->fields[j].type.type()->isSame(record[j].type.type()))) areFieldsSameType = false;
			}
			//Match the names to the corresponding record
			if(areFieldsSameType) return areFieldsUnnamed ? headRecord : findSubRecord(headRecord,(*i).second,record);
		}
	}

	//If the record doesn't exist we have to add it to head and sub records
	auto t = createRecordType(record);
	records.push_back(std::make_pair(t,std::vector<Record*>()));
	if(areFieldsUnnamed) return t;
	t = createRecordType(record,t);
	records.back().second.push_back(t);
	return t;
}

Trait::Trait() : DeclaredType(Type::VARIANT) {
}

DeclaredType* Trait::duplicate(DuplicationModifiers* mods) const {
	auto dup = new Trait();
	return dup;
}


/**
* Variant type
*/
Variant::Variant() : DeclaredType(Type::VARIANT) {
}
int Variant::lookupField(const SymbolID fieldName) const {
	for(size_t i = 0;i<fields.size();i++){
		if( fields[i].name == fieldName ) return int(i);
	}
	return -1;
}
void Variant::add(const Field& field) {
	assert(!isResolved());
	fields.push_back(field);
}
size_t Variant::size(){
	return _layout.size;
}
DeclaredType* Variant::duplicate(DuplicationModifiers* mods) const{
	auto dup = new Variant();
	dup->_resolved = _resolved;
	dup->_layout = _layout;
	return dup;
}
//TODO proper tag size.. and tag alignment 
void Variant::calculateResolvedProperties(){
	_layout.size   = fields.size() < 256? 1 : 4;
	auto sb = declaration->optionalStaticBlock;
	for(auto i = fields.begin();i!=fields.end();++i){
		if(i->associatedType != -1)
			_layout.size = std::max(_layout.size,(*(sb->begin()+i->associatedType))->asTypeDeclaration()->type()->size());
	}
}

TypeDeclaration::TypeDeclaration(DeclaredType* type,SymbolID name) : PrefixDefinition(name,Location()), _type(type),optionalStaticBlock(nullptr) { 
	_type->declaration = this; 
}
Type*  TypeDeclaration::type()  const { 
	return _type; 
}
Node*  TypeDeclaration::createReference(){
	return new TypeReference(_type);
}
Node*  TypeDeclaration::duplicate(DuplicationModifiers* mods) const {
	BlockExpression* sb = optionalStaticBlock? optionalStaticBlock->duplicate(mods)->asBlockExpression() : nullptr;
	auto dup = new TypeDeclaration(_type->duplicate(mods),label());
	dup->optionalStaticBlock = sb;
	mods->duplicateDefinition(const_cast<TypeDeclaration*>(this),dup);
	return copyProperties(dup);
}
