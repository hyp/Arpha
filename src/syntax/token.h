/**
* This module defines a token.
* A token is either a symbol, newline, number literal, string literal, comment or an EOF. 
*/
#ifndef ARPHA_TOKEN_H
#define ARPHA_TOKEN_H

#include "../base/base.h"
#include "../base/symbol.h"

struct Token {
	enum {
		Symbol = 0,
		Uinteger,
		EndExpression,
		Eof,
	};
	
	union {
		uint64 uinteger;
		double real;
	};
	SymbolID symbol;	
	int type;
	
	Token();
	inline bool isSymbol() const { return type == Symbol; }
	inline bool isUinteger() const { return type == Uinteger; }
	inline bool isEOF() const { return type == Eof; }
	inline bool isEndExpression() const { return type == EndExpression; }
};

std::ostream& operator<< (std::ostream& stream,const Token& token);

#endif