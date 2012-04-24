#include "token.h"

Token::Token(){
	type = -1;	
	uinteger = 0;	
}

std::ostream& operator<< (std::ostream& stream,const Token& token){
	if(token.isSymbol()) stream<<token.symbol.ptr();
	else if(token.isUinteger()) stream<<token.uinteger;
	else if(token.isEndExpression()) stream<<"';'";
	else if(token.isEOF()) stream<<"EOF";
	else throw std::runtime_error("Token is of unknown type!");
	return stream;
}

unittest(token){
	Token token;
	token.type = Token::Symbol;
	assert(token.isSymbol());
	token.type = Token::Eof;
	assert(token.isEOF());
}