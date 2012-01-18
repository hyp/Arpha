#include "common.h"
#include "scope.h"
#include "declarations.h"
#include "parser.h"

#include "interpreter.h"
#include "ast.h"
#include "compiler.h"
#include "arpha.h"

//expression evaluation - resolving overloads, inferring types, invoking ctfe

Node* evaluateResolvedFunctionCall(Parser* parser,CallExpression* node){
	auto function = ((ConstantExpression*)node->object)->refFunction;
	bool interpret = false;

	if(function->argument == compiler::expression) interpret = true;
	//else if(node->arg->is<TypeExpression>()) interpret = true;

	
	if(!interpret) return node;
	debug("Interpreting function call %s with %s",function->id,node->arg);
	Interpreter interpreter;
	interpreter.expressionFactory = parser->expressionFactory;
	return interpreter.interpret(node);
	return node;
}

inline Node* evaluate(Parser* parser,CallExpression* node){
#define CASE(t) case t::__value__
	node->arg = parser->evaluate(node->arg);
	auto argumentType = returnType(node->arg);
	
	if(argumentType == compiler::Nothing) error(node->arg->location,"Can't perform function call on a statement!");
	else if(argumentType != compiler::Unresolved){
		if(node->object->is<OverloadSetExpression>()){
			auto func = ((OverloadSetExpression*)node->object)->scope->resolveFunction(((OverloadSetExpression*)node->object)->symbol,node->arg);
			if(func){
				node->object = parser->expressionFactory->makeFunctionReference(func);
				//TODO function->adjustArgument
				debug("Overload successfully resolved as %s: %s",func->id,func->argument->id);
				return evaluateResolvedFunctionCall(parser,node);
			}else{
				//TODO mark current block as unresolved!
			}
		}else
			error(node->object->location,"Can't perform a function call onto %s!",node->object);
	}

	return node;
#undef CASE
}

inline Node* evaluate(Parser* parser,TupleExpression* node){
	if(node->children.size() == 0){ node->type= arpha::Nothing; return node; }
	
	std::vector<std::pair<SymbolID,Type*>> fields;
	
	node->type = nullptr;
	Type* returns;
	for(size_t i =0;i<node->children.size();i++){
		node->children[i] = parser->evaluate( node->children[i] );
		returns = returnType(node->children[i]);

		if(returns == compiler::Nothing){
			error(node->children[i]->location,"a tuple can't have a statement member");
			node->type = compiler::Error;
		}
		else if(returns == compiler::Unresolved) node->type = compiler::Unresolved;
		else fields.push_back(std::make_pair(SymbolID(),returns));
	}

	if(!node->type) node->type = Type::tuple(fields);
	return node;
}

inline Node* evaluate(Parser* parser,BlockExpression* node){
	for(size_t i =0;i<node->children.size();i++)
		node->children[i] = parser->evaluate( node->children[i] );
	return node;
}

#define CASE(t) case t::__value__: node = ::evaluate(this,(t*)node); break

Node* Parser::evaluate(Node* node){

	switch(node->__type){
		CASE(CallExpression);
		CASE(TupleExpression);
		CASE(BlockExpression);
		
	}

	return node;
}

#undef CASE