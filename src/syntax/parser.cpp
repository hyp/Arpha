#include "../base/base.h"
#include "../base/bigint.h"
#include "../scope.h"
#include "../ast/declarations.h"
#include "../ast/node.h"
#include "parser.h"
#include "../compiler.h"

Parser::Parser(const char* src) : Lexer(src) {  
}

void Parser::currentScope(Scope* scope){
	_currentScope=scope;
	firstRoundEvaluator.currentScope(scope);
}

void Parser::expect(SymbolID token){
	Token tok = consume();
	if(tok.isSymbol()==false || tok.symbol!=token){
		error(previousLocation(),"'%s' expected!",token);
	}
}

bool Parser::match(SymbolID token){
	Token tok = peek();
	if(tok.isSymbol()==false || tok.symbol!=token) return false;
	consume();
	return true;
}

bool Parser::match(int tokenType){
	assert(tokenType >= Token::Symbol && tokenType <= Token::Eof); 
	Token tok = peek();
	if(tok.type != tokenType) return false;
	consume();
	return true;
}

SymbolID Parser::expectName(){
	Token tok = consume();
	if(tok.isSymbol()==false){
		error(previousLocation(),"A valid name is expected!");
		return SymbolID("error-name");
	}
	return tok.symbol;
}

Node* Parser::evaluate(Node* node){
	return firstRoundEvaluator.eval(node);
}


static Node* parseNotSymbol(Parser* parser){
	Token& token = parser->lookedUpToken;
	if(token.isUinteger()){
		return new IntegerLiteral(BigInt(token.uinteger));
	}else{
		error(parser->previousLocation(),"Unexpected token %s!",token);
		return ErrorExpression::getInstance();
	}
}

/**
* Pratt parser is fucking awsome.
*/
Node* Parser::parse(int stickiness){
	Node* expression;

	//prefix	
	auto location = currentLocation();
	lookedUpToken = consume();
	if(lookedUpToken.isSymbol()){
		auto prefixDefinition = _currentScope->lookupPrefix(lookedUpToken.symbol);
		if(!prefixDefinition){ 
			error(location,"Can't prefix parse %s!",lookedUpToken); //TODO unresolved name
			expression = ErrorExpression::getInstance();
		}else{
			expression = prefixDefinition->parse(this);
		}
	}
	else expression = parseNotSymbol(this);
	expression->location = location;
	expression = evaluate(expression);

	//infix parsing
	while(1){
		lookedUpToken = peek();
		if(lookedUpToken.isSymbol()){
			auto infixDefinition = _currentScope->lookupInfix(lookedUpToken.symbol);
			if(infixDefinition && stickiness < infixDefinition->stickiness){
				location = currentLocation();
				consume();
				expression = infixDefinition->parse(this,expression);
				expression->location = location;
				expression = evaluate(expression);
			}
			else break;
		}else break;	
	}	
	return expression;	
}


//parsing declarations

Node* ImportedScope::parse(Parser* parser) {
	return nullptr;//TODo ConstantExpression::createScopeReference(scope);
}

void InferredUnresolvedTypeExpression::parse(Parser* parser,int stickiness){
	auto oldSetting = parser->evaluator()->evaluateTypeTuplesAsTypes;
	parser->evaluator()->evaluateTypeTuplesAsTypes = true;
	auto node = parser->parse(stickiness);
	parser->evaluator()->evaluateTypeTuplesAsTypes = oldSetting;

	auto isTypeExpr = node->asTypeExpression();
	if(isTypeExpr && isTypeExpr->resolved()){
		kind = Type;
		_type = isTypeExpr;
		return;
	}
	kind = Unresolved;
	unresolvedExpression = node;
}

Node* Variable::parse(Parser* parser){
	return reference();
}

Node* Record::parse(Parser* parser){
	return reference();
}

Node* IntegerType::parse(Parser* parser){
	return reference();
}

Node* IntrinsicType::parse(Parser* parser){
	return reference();
}

Node* Function::parse(Parser* parser){
	return nullptr;//FunctionReference::create(this);//TODO remove?
}

Node* Overloadset::parse(Parser* parser){
	return OverloadSetExpression::create(parser->lookedUpToken.symbol,parser->currentScope());
}

Node* PrefixOperator::parse(Parser* parser){
	return CallExpression::create(OverloadSetExpression::create(function,parser->currentScope()),parser->parse());
}

Node* InfixOperator::parse(Parser* parser,Node* node){
	auto tuple = new TupleExpression;
	tuple->children.push_back(node);
	tuple->children.push_back(parser->parse(stickiness));
	return CallExpression::create(OverloadSetExpression::create(function,parser->currentScope()),tuple);
}
