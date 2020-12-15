/*
 * TaskBase.hpp
 *
 *  Created on: 11 Dec 2020
 *      Author: Kamtar
 */

#ifndef MISC_TASKBASE_HPP_
#define MISC_TASKBASE_HPP_

#include "stdint.h"
#include "assert.h"


class TaskBase {
public:
	virtual bool Init() { assert(false); }
	virtual void Tick_ms(uint32_t elapsed_ms) { assert(false); }
	virtual void Tick_sec(uint32_t elapsed_ms) { assert(false); }
	virtual bool Task() { assert(false); }
};

#endif /* MISC_TASKBASE_HPP_ */
