/*
 *	Copyright 2021 Andrey Terekhov, Victor Y. Fadeev, Dmitrii Davladov
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#pragma once

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Hash table */
typedef struct map_hash map_hash;

/** Associative array (Dictionary) */
typedef struct map
{
	map_hash *table;			/**< Key-values hash table */
	size_t size;				/**< Size of table */
	size_t alloc;				/**< Allocated size of table */
} map;


/**
 *	Create map structure
 *
 *	@param	values			Initializer of values count
 *
 *	@return	Map structure
 */
map map_create(const size_t values);


/**
 *	Add new key-value pair
 *
 *	@param	as				Map structure
 *	@param	key				Unique string key
 *	@param	value			Value
 *
 *	@return	Index of record, @c SIZE_MAX on failure
 */
size_t map_add(map *const as, const char *const key, const int value);

/**
 *	Set new value by existing key
 *
 *	@param	as				Map structure
 *	@param	key				Unique string key
 *	@param	value			New value
 *
 *	@return	Index of record, @c SIZE_MAX on failure
 */
size_t map_set(map *const as, const char *const key, const int value);

/**
 *	Set new value by index
 *
 *	@param	as				Map structure
 *	@param	index			Record index
 *	@param	value			New value
 *
 *	@return	@c 0 on success, @c -1 on failure
 */
int map_set_at(map *const as, const size_t index, const int value);


/**
 *	Get value by key
 *
 *	@param	as				Map structure
 *	@param	key				Unique string key
 *
 *	@return	Value, @c INT_MAX on failure
 */
int map_get(const map *const as, const char *const key);

/**
 *	Get value by index
 *
 *	@param	as				Map structure
 *	@param	index			Value index
 *
 *	@return	Value, @c INT_MAX on failure
 */
int map_get_at(const map *const as, const size_t index);


/**
 *	Free allocated memory
 *
 *	@param	as				Map structure
 *
 *	@return	@c 0 on success, @c -1 on failure
 */
int map_clear(map *const as);

#ifdef __cplusplus
} /* extern "C" */
#endif
