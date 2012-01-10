#ifndef ARPHA_H
#define ARPHA_H

namespace arpha {
	void test();
	void init();

	//core types
	Type* type;
	Type* expression;
	Type* Nothing,*Unresolved,*inferred;
	Type *constant;
	Type *int8,*uint8,*int16,*uint16,*int32,*uint32,*int64,*uint64;
	Type *float64,*float32;
	Type *boolean;	

	inline bool isReal(Type* type){
		return type == float32 || type == float64;
	}
	inline bool isInteger(Type* type){
		return type==int32 || type==uint32 || type == int64|| type == uint64 || type==int16 || type==uint16 || type==int8 || type==uint8;
	}
	inline bool isSignedInteger(Type* type){
		return type==int32 || type == int64 ||  type==int16 ||  type==int8;
	}
	inline bool isAssignableFromConstant(Type* type){
		return type==int32 || type==uint32 || type == int64 || type == uint64 || type==int16 || type==uint16 || type==int8 || type==uint8 || type == float32 || type ==float64 || type==boolean;
	}
};

#endif