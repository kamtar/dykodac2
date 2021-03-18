/*
 * MamaGpio.hpp
 *
 *  Created on: 2 Mar 2021
 *      Author: Kamtar
 */

#ifndef LIB_CPP_MAMA_HAL_MAMAGPIO_HPP_
#define LIB_CPP_MAMA_HAL_MAMAGPIO_HPP_

class MamaGpio
{
public:

	__attribute__((weak)) inline void Write(bool st) { assert(false); };
	__attribute__((weak)) inline bool Read() { assert(false); return false;};
	__attribute__((weak)) inline bool GetState() { assert(false); return false; };

	inline void Set(){ Write(true); };
	inline void Clear() { Write(false); };
	inline void Toogle() { Write(!GetState()); };
};



#endif /* LIB_CPP_MAMA_HAL_MAMAGPIO_HPP_ */
