
#pragma once

/****************************************************************************
 Copyright (c) 2014 Chukong Technologies Inc.
 Author: Justin Graham (https://github.com/mannewalis)
 
 http://www.cocos2d-x.org
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "pthread.h"
#include <memory>

template<typename Lambda>
std::shared_ptr<void> voidify(Lambda&& l) 
{
  typedef typename std::decay<Lambda>::type Func;
  std::shared_ptr<void> data = std::make_shared<Func>(std::forward<Lambda>(l));
  return std::move(data);
}

template <typename T>
class TLS
{
public:

	explicit TLS(size_t count = 1, size_t size = sizeof(T))
		: _size(size)
		, _count(count)
		, _key()
		, _key_once(PTHREAD_ONCE_INIT)
	{
	}

	T& get(const size_t ndx = 0)
	{
		auto destruct = [](void* value) { free(value); };
		auto once = voidify([this, destruct]() { pthread_key_create(&this->_key, destruct); });
	    pthread_once(&_key_once, (void(*)())once.get());
        T* ptr;
		if (0 == (ptr = (T*)pthread_getspecific(_key))) 
        	ptr = (T*)malloc(_size * _count);
		return ptr[ndx];
	}

	void set(T& value, const size_t ndx = 0)
	{
		T* ptr = &get(ndx);
  		memcpy(ptr, &value, _size);
  		pthread_setspecific(_key, ptr);
    }

	T& operator[] (const int ndx)
	{
	    return get(ndx);
	}

protected:

	size_t _size;
	size_t _count;
	pthread_key_t _key;
	pthread_once_t _key_once;
};
