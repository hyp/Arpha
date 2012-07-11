#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Bitcode/ReaderWriter.h"

#include "../../base/base.h"
#include "../../base/symbol.h"
#include "../../base/bigint.h"
#include "../../base/format.h"

#include "../../syntax/location.h"
#include "../../ast/scope.h"
#include "../../ast/node.h"
#include "../../ast/declarations.h"
#include "../../ast/visitor.h"

#include "gen.h"
#include "../mangler.h"

/**
* Globals
*/
enum {
	GEN_TYPE_I8,
	GEN_TYPE_I16,
	GEN_TYPE_I32,
	GEN_TYPE_I64,
	GEN_TYPE_SIZE_T,
	GEN_TYPE_VOID,
	GEN_TYPE_FLOAT,
	GEN_TYPE_DOUBLE,
	GEN_MAX_CORE_TYPES
};

namespace {
	llvm::Type* coreTypes[GEN_MAX_CORE_TYPES];
	llvm::TargetMachine* targetMachine;
};

/**
* LLVM IR construction.
*/
struct LLVMgenerator: NodeVisitor {
	llvm::IRBuilder<>  builder;
	llvm::LLVMContext& context;
	llvm::Module*      module;
	llvm::Value*       emmittedValue;
	gen::Mangler*      moduleMangler;
	Function* functionOwner;
	std::vector<Node*> moduleInitializerBody;

	/**
	* Control flow chains
	*/
	//for building loops
	struct LoopChainNode {
		LoopExpression* loop;
		llvm::BasicBlock* body;
		llvm::BasicBlock* after;
		LoopChainNode* prev;
	};
	LoopChainNode* loopChain;
	//for building if fallthroughs
	bool ifFallthrough;

	llvm::FunctionPassManager* passManager;

	LLVMgenerator(llvm::LLVMContext& _context,Node* root,llvm::Module* module,llvm::FunctionPassManager* passManager);
	llvm::Type* genType(Type* type);
	void emitConstant(llvm::Constant* value);
	inline void emit(llvm::Value* value){ emmittedValue = value; }
	llvm::Value* generateExpression(Node* node);
	static llvm::GlobalValue::LinkageTypes genLinkage(PrefixDefinition* def);

	inline static void map(DefinitionNode* def,llvm::Value* value){
		def->generatorData = value;
	}
	inline static llvm::Value* unmap(DefinitionNode* def){
		return reinterpret_cast<llvm::Value*>(def->generatorData);
	}
	inline static void map(DeclaredType* type,llvm::Type* value){
		type->generatorData = value;
	}
	inline static llvm::Type* unmap(DeclaredType* type){
		return reinterpret_cast<llvm::Type*>(type->generatorData);
	}
	inline void emitLoad(llvm::Value* value){
		emmittedValue = builder.CreateLoad(value);
	}
	inline void emitStore(llvm::Value* var,llvm::Value* val){
		emmittedValue = builder.CreateStore(val,var);
	}

	//
	Node* visit(IntegerLiteral* node);
	Node* visit(BoolExpression* node);
	Node* visit(VariableReference* node);
	Node* visit(UnaryOperation* node);
	Node* visit(BinaryOperation* node);
	Node* visit(AssignmentExpression* node);
	Node* visit(FieldAccessExpression* node);
	Node* visit(CallExpression* node);
	Node* visit(PointerOperation* node);
	Node* visit(ReturnExpression* node);
	Node* visit(IfExpression* node);
	Node* visit(ControlFlowExpression* node);
	Node* visit(LoopExpression* node);
	Node* visit(BlockExpression* node);
	void  gen(BlockExpression* node);

	Node* visit(Variable* var);
	Node* visit(Function* func);
	Node* visit(TypeDeclaration* node);

	llvm::GlobalVariable* getGlobalVariableDeclaration(Variable* variable);
	inline llvm::Value*   getVariable(Variable* variable) {
		return variable->isLocal()? unmap(variable): getGlobalVariableDeclaration(variable);
	}
	llvm::Function* getFunctionDeclaration(Function* function);
};


LLVMgenerator::LLVMgenerator(
	llvm::LLVMContext& _context,
	Node* root,llvm::Module* module,
	llvm::FunctionPassManager* passManager) : 
	context(_context),
	builder(_context) 
{
	this->module     = module;
	this->passManager= passManager;

	emmittedValue = nullptr;
	functionOwner = nullptr;
	loopChain = nullptr;
	ifFallthrough = false;

	//create a module initializer

	gen(root->asBlockExpression());
}

llvm::Type* generateAnonymousRecord(LLVMgenerator* generator,AnonymousAggregate* type){
	std::vector<llvm::Type*> fields(type->numberOfFields);
	for(size_t i = 0;i<type->numberOfFields;i++){
		fields[i] = generator->genType(type->types[i]);
	}
	return llvm::StructType::get(generator->context,fields,false);
}

llvm::Type* getRecordDeclaration(LLVMgenerator* generator,Record* record){
	if(record->generatorData) return generator->unmap(record);

	//mangle
	gen::Mangler::Element mangler(generator->moduleMangler);
	mangler.mangle(record->declaration);

	auto numberOfFields = record->fields.size();
	std::vector<llvm::Type*> fields(numberOfFields);
	for(size_t i = 0;i<numberOfFields;i++){
		fields[i] = generator->genType(record->fields[i].type.type());
	}
	auto t = llvm::StructType::create(generator->context,fields,mangler.stream.str(),false);	
	generator->map(record,t);
	return t;
}

llvm::Type* generatePointerType(LLVMgenerator* generator,Type* next){
	return llvm::PointerType::get(generator->genType(next),0);
}

// { T* ptr,natural length }
llvm::Type* generateLinearSequence(LLVMgenerator* generator,Type* next){
	llvm::Type* fields[2] = { generatePointerType(generator,next),coreTypes[GEN_TYPE_SIZE_T] };
	return llvm::StructType::get(generator->context,fields,true);//NB: packed
}

Node* LLVMgenerator::visit(TypeDeclaration* node){
	return node;
}

llvm::Type* LLVMgenerator::genType(Type* type){
	switch(type->type){
	case Type::VOID: return llvm::Type::getVoidTy(context);
	case Type::TYPE: assert(false); break;
	case Type::BOOL: return llvm::Type::getInt8Ty(context);
	case Type::RECORD:
		return getRecordDeclaration(this,static_cast<Record*>(type));
	case Type::FLOAT:
		return type->bits == 32? llvm::Type::getFloatTy(context) : llvm::Type::getDoubleTy(context);
	case Type::CHAR:
		return coreTypes[type->bits == 8? GEN_TYPE_I8 : (type->bits == 32? GEN_TYPE_I32 : GEN_TYPE_I16)];
	case Type::POINTER:
	case Type::POINTER_BOUNDED:
	case Type::POINTER_BOUNDED_CONSTANT:
		return generatePointerType(this,type->next());
	case Type::NODE: assert(false); break;
	case Type::ANONYMOUS_RECORD:
		return generateAnonymousRecord(this,static_cast<AnonymousAggregate*>(type));
	case Type::LINEAR_SEQUENCE:
		return generateLinearSequence(this,type->next());
	case Type::LITERAL_STRING:
		assert(false); break;
	}
	return coreTypes[0];
}
void LLVMgenerator::emitConstant(llvm::Constant* value){
	emmittedValue = value;
}
llvm::Value* LLVMgenerator::generateExpression(Node* node){
	emmittedValue = nullptr;
	node->accept(this);
	return emmittedValue;
}
llvm::GlobalValue::LinkageTypes LLVMgenerator::genLinkage(PrefixDefinition* def){
	return def->isPublic() ? llvm::GlobalValue::LinkageTypes::ExternalLinkage : llvm::GlobalValue::LinkageTypes::PrivateLinkage;
}


Node* LLVMgenerator::visit(IntegerLiteral* node){
	emitConstant(llvm::ConstantInt::get(coreTypes[GEN_TYPE_I8],node->integer.u64,false));
	return node;
}

Node* LLVMgenerator::visit(CharacterLiteral* node){
	emitConstant(llvm::ConstantInt::get(generateType(node->returnType()),node->value,false));
	return node;
}

Node* LLVMgenerator::visit(BoolExpression* node){
	emitConstant(llvm::ConstantInt::get(coreTypes[GEN_TYPE_I8],node->value,false));
	return node;
}

Node* LLVMgenerator::visit(VariableReference* node){
	emitLoad(getVariable(node->variable));
	return node;
}

Node* LLVMgenerator::visit(UnaryOperation* node){
	auto value = generateExpression(node->expression);
	llvm::Value* instr;
	switch(node->kind()){
	case UnaryOperation::BOOL_NOT:
		instr =builder.CreateXor(value,1);
		break;
	case UnaryOperation::MINUS:
		//builder.CreateFNeg(value);
		instr =builder.CreateNeg(value);
		break;
	case UnaryOperation::BOUNDED_POINTER_LENGTH:
		//TODO 2nd field
		break;
	}
	emit(instr);
	return node;
}

//ptr + 0
llvm::Value* genDereference(LLVMgenerator* generator,PointerOperation* node){
	auto ptr = generator->generateExpression(node->expression);
	auto idx = llvm::ConstantInt::get(coreTypes[GEN_TYPE_I8],0,false);
	return generator->builder.CreateGEP(ptr,idx);
}
//ptr + i
llvm::Value* genPointerAddressing(LLVMgenerator* generator,Node* object,Node* index){
	auto ptr = generator->generateExpression(object);
	auto idx = generator->generateExpression(index);
	return generator->builder.CreateGEP(ptr,idx);
}

Node* LLVMgenerator::visit(BinaryOperation* node){
	llvm::Value* instr;
	if(node->kind() == BinaryOperation::BOUNDED_POINTER_ELEMENT){
		emitLoad(genPointerAddressing(this,node->a,node->b));//TODO first field from bp
		return node;
	}
	bool isFloatOp = false;
	auto lhs = generateExpression(node->a);
	auto rhs = generateExpression(node->b);
	switch(node->kind()){
	case BinaryOperation::EQUALS:
		if(!isFloatOp)
			instr =builder.CreateICmpEQ(lhs,rhs);
		else 
			instr =builder.CreateFCmpOEQ(lhs,rhs);
		break;
	case BinaryOperation::ADD:
		if(!isFloatOp)
			instr =builder.CreateAdd(lhs,rhs);
		else 
			instr =builder.CreateFAdd(lhs,rhs);
		break;
	case BinaryOperation::SUBTRACT:
		if(!isFloatOp)
			instr =builder.CreateSub(lhs,rhs);
		else 
			instr =builder.CreateFSub(lhs,rhs);
		break;
	case BinaryOperation::MULTIPLY:
		if(!isFloatOp)
			instr =builder.CreateMul(lhs,rhs);
		else 
			instr =builder.CreateFMul(lhs,rhs);
		break;
	}
	emit(instr);
	return node;
};
Node* LLVMgenerator::visit(AssignmentExpression* node){
	auto val = generateExpression(node->value);
	assert(val);
	if(auto var = node->object->asVariableReference()){
		emitStore(getVariable(var->variable),val);
	} else if(auto field = node->object->asFieldAccessExpression()){
		if(auto vref = field->object->asVariableReference()){
			auto variable = unmap(vref->variable);
			auto fieldPtr = builder.CreateStructGEP(variable,field->field);
			emitStore(fieldPtr,val);
		}
	}
	return node;
}
Node* LLVMgenerator::visit(FieldAccessExpression* node){
	if(auto vref = node->object->asVariableReference()){
		auto variable = unmap(vref->variable);
		auto fieldPtr = builder.CreateStructGEP(variable,node->field);
		emitLoad(fieldPtr);
	}
	return node;
}

//TODO cc for function pointers
Node* LLVMgenerator::visit(CallExpression* node){
	llvm::Value* callee;
	llvm::CallInst* instr;
	const char* reg;

	if(auto fcall = node->object->asFunctionReference()){
		reg = fcall->function->returns()->isVoid()? "" : "calltmp";
		callee = getFunctionDeclaration(fcall->function);
	}
	else {
		reg = "calltmp";
		callee = generateExpression(node->object); //calling a function pointer
	}
	
	llvm::Twine twine(reg);
	if(node->arg->asUnitExpression()){
		instr = builder.CreateCall(callee,twine);
	}
	else if(auto tuple = node->arg->asTupleExpression()) {
		auto argCount = tuple->size();
		if(argCount == 2){
			instr = builder.CreateCall2(callee,generateExpression(*(tuple->begin())),generateExpression(*(tuple->begin()+1)),twine);
		} else {
			std::vector<llvm::Value*> args;
			args.reserve(argCount);
			for(auto i = tuple->begin();i!=tuple->end();i++){
				args.push_back(generateExpression(*i));
			}	
			instr = builder.CreateCall(callee,args,twine);
		}
	}
	else {
		instr = builder.CreateCall(callee,generateExpression(node->arg),twine);
	}
	//TODO cc for function pointers: instr->setCallingConv();

	emit(instr);
	return node;
}

Node* LLVMgenerator::visit(PointerOperation* node){
	if(node->isAddress()){
		auto expr = node->expression;
		if(auto var = expr->asVariableReference()){
			emit(unmap(var->variable));
		}
	} else {
		emitLoad(genDereference(this,node));
	}
	return node;
}
Node* LLVMgenerator::visit(ReturnExpression* node){
	if(!functionOwner->_returnType.type()->isVoid()){
		builder.CreateRet(generateExpression(node->expression));
	}
	else {
		if(!node->expression->asUnitExpression()) generateExpression(node->expression);
		builder.CreateRetVoid();
	}
	return node;
}
Node* LLVMgenerator::visit(IfExpression* node){
	bool returnsValue   = false;
	bool isSelect = returnsValue && !node->consequence->asBlockExpression() && !node->alternative->asBlockExpression();
	
	//condition
	auto cond = generateExpression(node->condition);

	//if(true) 1 else 2
	if(isSelect){
		emit(builder.CreateSelect(cond,generateExpression(node->consequence),generateExpression(node->alternative)));
		return node;
	}

	//More complex if
	llvm::BasicBlock* alternativeBlock = nullptr;
	llvm::Value* consequenceValue,*alternativeValue;
	auto f = builder.GetInsertBlock()->getParent();

	//branch
	auto consequenceBlock = llvm::BasicBlock::Create(context, "then", f);
	auto mergingBlock     = llvm::BasicBlock::Create(context,"ifcont");
	if(!node->alternative->asUnitExpression()){
		alternativeBlock = llvm::BasicBlock::Create(context,"else");
		builder.CreateCondBr(cond,consequenceBlock,alternativeBlock);
	}
	else builder.CreateCondBr(cond,consequenceBlock,mergingBlock);

	ifFallthrough = false;

	//consequence
	builder.SetInsertPoint(consequenceBlock);
	if(returnsValue) consequenceValue = generateExpression(node->consequence);
	consequenceBlock = builder.GetInsertBlock();//update consequence block
	if(ifFallthrough){
		assert(alternativeBlock);
		builder.CreateBr(alternativeBlock);
		ifFallthrough = false;
	} else builder.CreateBr(mergingBlock);
	//alternative
	if(alternativeBlock){
		f->getBasicBlockList().push_back(alternativeBlock);
		builder.SetInsertPoint(alternativeBlock);
		builder.CreateBr(mergingBlock);
		if(returnsValue) alternativeValue = generateExpression(node->alternative);
		alternativeBlock = builder.GetInsertBlock();//update alternative block
	}
	//merge
	f->getBasicBlockList().push_back(mergingBlock);
	builder.SetInsertPoint(mergingBlock);
	if(returnsValue){
		auto phi = builder.CreatePHI(genType(node->returnType()),2,"iftmp");
		phi->addIncoming(consequenceValue,consequenceBlock);
		phi->addIncoming(alternativeValue,alternativeBlock);
		emit(phi);
	}
	return node;
}
Node* LLVMgenerator::visit(ControlFlowExpression* node){
	if(node->isBreak()){
		builder.CreateBr(loopChain->after);
	} else if(node->isContinue()){
		builder.CreateBr(loopChain->body);
	} else if(node->isFallthrough()){
		ifFallthrough = true;
	}
	return node;
}
Node* LLVMgenerator::visit(LoopExpression* node){
	//body
	auto preBlock  = builder.GetInsertBlock();
	auto loopBlock = llvm::BasicBlock::Create(context,"loop",preBlock->getParent());
	builder.CreateBr(loopBlock);
	builder.SetInsertPoint(loopBlock);
	//after
	auto afterBlock = llvm::BasicBlock::Create(context,"loopcont");
	
	LoopChainNode loopNode = { node,loopBlock,afterBlock,loopChain };
	loopChain = &loopNode;
	if(auto block = node->body->asBlockExpression()){
		gen(block);
	} else generateExpression(node->body);
	loopChain = loopNode.prev;

	//merge
	preBlock->getParent()->getBasicBlockList().push_back(afterBlock);
	builder.SetInsertPoint(afterBlock);
	
	return node;
}

void  LLVMgenerator::gen(BlockExpression* node){
	for(auto i = node->begin();i!=node->end();i++){
		generateExpression(*i);
	}
}

Node* LLVMgenerator::visit(BlockExpression* node){
	gen(node);
	return node;
}

llvm::GlobalVariable*  LLVMgenerator::getGlobalVariableDeclaration(Variable* variable){
	if(variable->generatorData) return static_cast<llvm::GlobalVariable*>(unmap(variable));

	// mangle
	gen::Mangler::Element mangler(moduleMangler);
	mangler.mangle(variable);

	auto threadLocal = true;
	auto cnst = false;
	llvm::Constant* init = llvm::ConstantInt::get(coreTypes[GEN_TYPE_I8],0,false);
	auto var = new llvm::GlobalVariable(*module,genType(variable->type.type()),cnst,genLinkage(variable),nullptr,mangler.stream.str(),nullptr,threadLocal);
	//var->setAlignment(variable->type.type()->alignment());
	map(variable,var);

	return var;
}

Node* LLVMgenerator::visit(Variable* variable){
	if(!functionOwner){
		auto var = getGlobalVariableDeclaration(variable);
	} else {
		auto block = builder.GetInsertBlock();
		llvm::IRBuilder<> _builder(block,block->begin());
		auto var = _builder.CreateAlloca(genType(variable->type.type()),nullptr,variable->label().ptr());
		map(variable,var);
	}
	return variable;
}

llvm::CallingConv::ID genCallingConvention(data::ast::Function::CallConvention cc){
	switch(cc){
	case data::ast::Function::ARPHA:   return llvm::CallingConv::C;//Fast;
	case data::ast::Function::CCALL:   return llvm::CallingConv::C;
	case data::ast::Function::STDCALL: return llvm::CallingConv::X86_StdCall;
	}
}

llvm::Function* LLVMgenerator::getFunctionDeclaration(Function* function){
	if(function->generatorData) return static_cast<llvm::Function*>(unmap(function));

	// mangle
	gen::Mangler::Element mangler(moduleMangler);
	mangler.mangle(function);

	//create the actual declaration
	llvm::FunctionType* t;
	if(function->arguments.size() == 0){
		t = llvm::FunctionType::get(genType(function->_returnType.type()),false);
	} else {
		std::vector<llvm::Type*> args;
		args.reserve(function->arguments.size());
		for(auto i = function->arguments.begin();i!=function->arguments.end();i++){
			args.push_back(genType((*i)->type.type()));
		}
		t = llvm::FunctionType::get(genType(function->_returnType.type()),args,false);
	}

	auto func = llvm::Function::Create(t,genLinkage(function),mangler.stream.str(),module);
	func->setCallingConv(genCallingConvention(function->callingConvention()));
	map(function,func);
	return func;
}

Node* LLVMgenerator::visit(Function* function){
	bool isExtern = function->label() == SymbolID("putchar");//ffiC;
	if(isExtern || function->isFlagSet(Function::MACRO_FUNCTION) || function->isFieldAccessMacro()) return function;

	auto func = getFunctionDeclaration(function);

	// Initialize arguments
	size_t i = 0;
	for (llvm::Function::arg_iterator arg = func->arg_begin(); i != function->arguments.size(); ++arg, ++i) {
		arg->setName(function->arguments[i]->label().ptr());
		map(function->arguments[i],arg);
	}

	//generate body
	llvm::BasicBlock *block = llvm::BasicBlock::Create(context, "entry", func);
	builder.SetInsertPoint(block);

	auto oldFunction = functionOwner;
	functionOwner = function;
	gen(&function->body);
	if(!function->isFlagSet(Function::CONTAINS_RETURN)) builder.CreateRetVoid();
	if(llvm::verifyFunction(*func,llvm::VerifierFailureAction::PrintMessageAction)){
		//TODO
	}
	passManager->run(*func);
	functionOwner = oldFunction;

	return function;
}


/*void llvmGenIntegerConstant (LLVMgenerator* generator,int type,uint64 value,bool isSigned){
	addExpr(generator,llvm::ConstantInt::get(LLVM_TYPE(type),value,isSigned));
}
void llvmGenFloatingConstant(LLVMgenerator* generator,int type,double value){
	addExpr(generator,llvm::ConstantFP::get(LLVM_TYPE(type),value));
}*/

/**
* LLVM backend implementation.
*/
namespace gen {

	static void createTargetTriple(LLVMBackend* backend,llvm::Triple *triple,data::gen::native::Target* target){
		using namespace data::gen::native;

		llvm::Triple::ArchType arch = llvm::Triple::ArchType::UnknownArch;
		switch(target->cpuArchitecture){
		case Target::Arch::X86:
			arch = llvm::Triple::ArchType::x86;
			break;
		case Target::Arch::ARM:
			arch = llvm::Triple::ArchType::arm;
			break;
		}
		triple->setArch(arch);
		if(target->cpuMode == Target::Mode::M64){
			*triple = triple->get64BitArchVariant();
			if(triple->getArch() == llvm::Triple::ArchType::UnknownArch) backend->onFatalError("The 64 bit mode isn't supported for the selected architecture!");
		}

		llvm::Triple::OSType os = llvm::Triple::OSType::UnknownOS;
		llvm::Triple::VendorType vendor = llvm::Triple::VendorType::UnknownVendor;
		switch(target->platform){
		case Target::Platform::WINDOWS:
		case Target::Platform::WINDOWS_RT:
			os = llvm::Triple::OSType::Win32;
			vendor = llvm::Triple::VendorType::PC;
			break;
		case Target::Platform::WINDOWS_MINGW:
			os = llvm::Triple::OSType::MinGW32;
			vendor = llvm::Triple::VendorType::PC;
			break;
		case Target::Platform::LINUX:
			os = llvm::Triple::OSType::Linux;
			break;
		case Target::Platform::MACOSX:
			os = llvm::Triple::OSType::Darwin;
			vendor = llvm::Triple::VendorType::Apple;
			break;
		}
		triple->setOS(os);
		triple->setVendor(vendor);

	}

	LLVMBackend::LLVMBackend(data::gen::native::Target* target,data::gen::Options* options){
		this->target  = target;
		this->options = options;

		llvm::LLVMContext& context = llvm::getGlobalContext();

		llvm::InitializeAllTargets();
		llvm::InitializeAllTargetMCs();
		llvm::InitializeAllAsmPrinters();
		llvm::InitializeAllAsmParsers();

		//todo target choosing
		llvm::Triple TheTriple;
		createTargetTriple(this,&TheTriple,target);

		const llvm::Target *TheTarget = 0;
		std::string Err;
		TheTarget = llvm::TargetRegistry::lookupTarget(TheTriple.getTriple(), Err);
		if (!TheTarget) {
			this->onFatalError(format("LLVM target: Couldn't find the target '%s'",TheTriple.str()));
		}

		auto MCPU = "";
		auto CMModel = llvm::CodeModel::Model::Default;
		auto RelocModel = llvm::Reloc::Default;
		//features
		std::string FeaturesStr;

		//
		int optimizationLevel = options->optimizationLevel;
		llvm::CodeGenOpt::Level OLvl = optimizationLevel == -1 ? llvm::CodeGenOpt::None : 
			optimizationLevel == 0 ? llvm::CodeGenOpt::Less : optimizationLevel == 1 ? llvm::CodeGenOpt::Default : llvm::CodeGenOpt::Aggressive;

		//target options
		llvm::TargetOptions  Options;
		Options.LessPreciseFPMADOption = false;
		Options.PrintMachineCode = false;
		Options.NoFramePointerElim = false;
		Options.NoFramePointerElimNonLeaf = false;
		Options.NoExcessFPPrecision = false;
		Options.UnsafeFPMath = false;
		Options.NoInfsFPMath = false;
		Options.NoNaNsFPMath = false;
		Options.HonorSignDependentRoundingFPMathOption = 0;
		Options.UseSoftFloat = false;
		Options.FloatABIType = llvm::FloatABI::Default;
		Options.NoZerosInBSS = false;
		Options.GuaranteedTailCallOpt = false;
		Options.DisableTailCalls = false;
		Options.StackAlignmentOverride = false;
		Options.RealignStack = true;
		Options.DisableJumpTables = false;
		Options.TrapFuncName = "";
		Options.PositionIndependentExecutable = false;
		Options.EnableSegmentedStacks = false;

		targetMachine = TheTarget->createTargetMachine(TheTriple.getTriple(),MCPU,FeaturesStr.c_str(),Options,RelocModel,CMModel,OLvl);

		//typesystem
		if(target->cpuMode == data::gen::native::Target::Mode::M32){
			target->typeSystemState.pointerSizeof = 4;
			target->typeSystemState.functionPointerSizeof = 4;
			target->typeSystemState.sizetSizeof = 4;
		} else {
			target->typeSystemState.pointerSizeof = 8;
			target->typeSystemState.functionPointerSizeof = 8;
			target->typeSystemState.sizetSizeof = 8;
		}

		coreTypes[GEN_TYPE_I8]  = llvm::Type::getInt8Ty(context);
		coreTypes[GEN_TYPE_I16] = llvm::Type::getInt16Ty(context);
		coreTypes[GEN_TYPE_I32] = llvm::Type::getInt32Ty(context);
		coreTypes[GEN_TYPE_I64] = llvm::Type::getInt64Ty(context);
		coreTypes[GEN_TYPE_SIZE_T] = target->typeSystemState.sizetSizeof == 8 ? llvm::Type::getInt64Ty(context) : llvm::Type::getInt32Ty(context);
		coreTypes[GEN_TYPE_VOID]   = llvm::Type::getVoidTy(context);
		coreTypes[GEN_TYPE_FLOAT]  = llvm::Type::getFloatTy(context);
		coreTypes[GEN_TYPE_DOUBLE] = llvm::Type::getDoubleTy(context);
	}


	//Generates a binary module
	static void genModule(LLVMBackend* backend,int outputFormat,llvm::Module* module,std::string& dest){
		//Open file
		std::string err;
		llvm::raw_fd_ostream out(dest.c_str(), err, llvm::raw_fd_ostream::F_Binary);
		if (!err.empty()){
			backend->onError(format("Couldn't open a file '%s' for writing!",dest));
			return;
		}
		//write instructions to file
		if(outputFormat == data::gen::native::ModuleOutputFormat::OBJECT || outputFormat == data::gen::native::ModuleOutputFormat::ASSEMBLY){
			using namespace llvm;
			llvm::Module& mod = *module;
			llvm::TargetMachine& Target = *targetMachine;
			llvm::TargetMachine::CodeGenFileType FileType = 
				outputFormat == data::gen::native::ModuleOutputFormat::OBJECT ? llvm::TargetMachine::CodeGenFileType::CGFT_ObjectFile : llvm::TargetMachine::CodeGenFileType::CGFT_AssemblyFile;
			llvm::raw_fd_ostream& Out = out;

			PassManager PM;
			
			// Add the target data from the target machine, if it exists, or the module.
			if (const TargetData *TD = Target.getTargetData())
				PM.add(new TargetData(*TD));
			else
				PM.add(new TargetData(&mod));

			Target.setAsmVerbosityDefault(true);
			Target.setMCRelaxAll(true);
			formatted_raw_ostream FOS(Out);

			// Ask the target to add backend passes as necessary.
			if (Target.addPassesToEmitFile(PM, FOS, FileType, true)) {
				backend->onFatalError("LLVM target doesn't support the generation of this file type!");
				return;
			}

			PM.run(mod);
		}
		else if(outputFormat == LLVMBackend::OUTPUT_BC ){
			llvm::WriteBitcodeToFile(module,out);
		}
	}
	
	std::string LLVMBackend::generateModule(Node* root,const char* outputDirectory,const char* moduleName,int outputFormat){
		using namespace llvm;
		auto module = new Module(StringRef(moduleName),getGlobalContext());
		auto passManager = new FunctionPassManager(module);
		// Add the target data from the target machine, if it exists, or the module.
		if (auto TD = targetMachine->getTargetData())
			passManager->add(new TargetData(*TD));
		else
			passManager->add(new TargetData(module));
		// Create various passes
		passManager->add(createPromoteMemoryToRegisterPass());
		int optimizationLevel = options->optimizationLevel;
		if(optimizationLevel >= 0){ 
			// Do simple "peephole" optimizations and bit-twiddling optzns.
			passManager->add(createInstructionCombiningPass());
			//Simplify the control flow graph (deleting unreachable blocks, etc).
			passManager->add(createCFGSimplificationPass());
		}
		if(optimizationLevel >= 1){
			// Provide basic AliasAnalysis support for GVN.
			passManager->add(createBasicAliasAnalysisPass());
			// Reassociate expressions.
			passManager->add(createReassociatePass());
			// Eliminate Common SubExpressions.
			passManager->add(createGVNPass());
		}
		passManager->doInitialization();
		
		LLVMgenerator generator(getGlobalContext(),root,module,passManager);
		module->dump();
		//extension
		const char* extension;
		bool isWinMSVS = target->platform == data::gen::AbstractTarget::Platform::WINDOWS || target->platform == data::gen::AbstractTarget::Platform::WINDOWS_RT;
		if(outputFormat == data::gen::native::ModuleOutputFormat::OBJECT)        extension =  isWinMSVS ? ".obj" : ".o";
		else if(outputFormat == data::gen::native::ModuleOutputFormat::ASSEMBLY) extension =  isWinMSVS ? ".asm" : ".S";
		else extension = ".bc";
		//
		std::string path = std::string(outputDirectory) + "/" + moduleName + extension;
		genModule(this,outputFormat,module,path);
		delete passManager;
		delete module;
		return path;
	}

};

