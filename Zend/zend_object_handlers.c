/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "zend.h"
#include "zend_globals.h"
#include "zend_variables.h"
#include "zend_API.h"
#include "zend_objects.h"
#include "zend_objects_API.h"
#include "zend_object_handlers.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "zend_closures.h"
#include "zend_compile.h"
#include "zend_hash.h"

#define DEBUG_OBJECT_HANDLERS 0

#define ZEND_WRONG_PROPERTY_OFFSET   0

/* guard flags */
#define IN_GET		(1<<0)
#define IN_SET		(1<<1)
#define IN_UNSET	(1<<2)
#define IN_ISSET	(1<<3)

/*
  __X accessors explanation:

  if we have __get and property that is not part of the properties array is
  requested, we call __get handler. If it fails, we return uninitialized.

  if we have __set and property that is not part of the properties array is
  set, we call __set handler. If it fails, we do not change the array.

  for both handlers above, when we are inside __get/__set, no further calls for
  __get/__set for this property of this object will be made, to prevent endless
  recursion and enable accessors to change properties array.

  if we have __call and method which is not part of the class function table is
  called, we cal __call handler.
*/

ZEND_API void rebuild_object_properties(zend_object *zobj) /* {{{ */
{
	if (!zobj->properties) {
		zend_property_info *prop_info;
		zend_class_entry *ce = zobj->ce;

		zobj->properties = zend_new_array(ce->default_properties_count);
		if (ce->default_properties_count) {
			zend_hash_real_init(zobj->properties, 0);
			zobj->properties->nInternalPointer = 0;
			ZEND_HASH_FOREACH_PTR(&ce->properties_info, prop_info) {
				if (/*prop_info->ce == ce &&*/
				    (prop_info->flags & ZEND_ACC_STATIC) == 0) {

					if (UNEXPECTED(Z_TYPE_P(OBJ_PROP(zobj, prop_info->offset)) == IS_UNDEF)) {
						zobj->properties->u.v.flags |= HASH_FLAG_HAS_EMPTY_IND;
					}

					_zend_hash_append_ind(zobj->properties, prop_info->name, 
						OBJ_PROP(zobj, prop_info->offset));
				}
			} ZEND_HASH_FOREACH_END();
			while (ce->parent && ce->parent->default_properties_count) {
				ce = ce->parent;
				ZEND_HASH_FOREACH_PTR(&ce->properties_info, prop_info) {
					if (prop_info->ce == ce &&
					    (prop_info->flags & ZEND_ACC_STATIC) == 0 &&
					    (prop_info->flags & ZEND_ACC_PRIVATE) != 0) {
						zval zv;

						if (UNEXPECTED(Z_TYPE_P(OBJ_PROP(zobj, prop_info->offset)) == IS_UNDEF)) {
							zobj->properties->u.v.flags |= HASH_FLAG_HAS_EMPTY_IND;
						}

						ZVAL_INDIRECT(&zv, OBJ_PROP(zobj, prop_info->offset));
						zend_hash_add(zobj->properties, prop_info->name, &zv);
					}
				} ZEND_HASH_FOREACH_END();
			}
		}
	}
}
/* }}} */

ZEND_API HashTable *zend_std_get_properties(zval *object) /* {{{ */
{
	zend_object *zobj;
	zobj = Z_OBJ_P(object);
	if (!zobj->properties) {
		rebuild_object_properties(zobj);
	}
	return zobj->properties;
}
/* }}} */

ZEND_API HashTable *zend_std_get_gc(zval *object, zval **table, int *n) /* {{{ */
{
	if (Z_OBJ_HANDLER_P(object, get_properties) != zend_std_get_properties) {
		*table = NULL;
		*n = 0;
		return Z_OBJ_HANDLER_P(object, get_properties)(object);
	} else {
		zend_object *zobj = Z_OBJ_P(object);

		if (zobj->properties) {
			*table = NULL;
			*n = 0;
			return zobj->properties;
		} else {
			*table = zobj->properties_table;
			*n = zobj->ce->default_properties_count;
			return NULL;
		}
	}
}
/* }}} */

ZEND_API HashTable *zend_std_get_debug_info(zval *object, int *is_temp) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zval retval;
	HashTable *ht;

	if (!ce->__debugInfo) {
		*is_temp = 0;
		return Z_OBJ_HANDLER_P(object, get_properties)
			? Z_OBJ_HANDLER_P(object, get_properties)(object)
			: NULL;
	}

	zend_call_method_with_0_params(object, ce, &ce->__debugInfo, ZEND_DEBUGINFO_FUNC_NAME, &retval);
	if (Z_TYPE(retval) == IS_ARRAY) {
		if (!Z_REFCOUNTED(retval)) {
			*is_temp = 1;
			return zend_array_dup(Z_ARRVAL(retval));
		} else if (Z_REFCOUNT(retval) <= 1) {
			*is_temp = 1;
			ht = Z_ARR(retval);
			return ht;
		} else {
			*is_temp = 0;
			zval_ptr_dtor(&retval);
			return Z_ARRVAL(retval);
		}
	} else if (Z_TYPE(retval) == IS_NULL) {
		*is_temp = 1;
		ht = zend_new_array(0);
		return ht;
	}

	zend_error_noreturn(E_ERROR, ZEND_DEBUGINFO_FUNC_NAME "() must return an array");

	return NULL; /* Compilers are dumb and don't understand that noreturn means that the function does NOT need a return value... */
}
/* }}} */

static void zend_std_call_getter(zval *object, zval *member, zval *retval) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_class_entry *orig_fake_scope = EG(fake_scope);

	EG(fake_scope) = NULL;

	/* __get handler is called with one argument:
	      property name

	   it should return whether the call was successful or not
	*/
	zend_call_method_with_1_params(object, ce, &ce->__get, ZEND_GET_FUNC_NAME, retval, member);

	EG(fake_scope) = orig_fake_scope;
}
/* }}} */

static void zend_std_call_setter(zval *object, zval *member, zval *value) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_class_entry *orig_fake_scope = EG(fake_scope);

	EG(fake_scope) = NULL;

	/* __set handler is called with two arguments:
	     property name
	     value to be set
	*/
	zend_call_method_with_2_params(object, ce, &ce->__set, ZEND_SET_FUNC_NAME, NULL, member, value);

	EG(fake_scope) = orig_fake_scope;
}
/* }}} */

static void zend_std_call_unsetter(zval *object, zval *member) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_class_entry *orig_fake_scope = EG(fake_scope);

	EG(fake_scope) = NULL;

	/* __unset handler is called with one argument:
	      property name
	*/

	zend_call_method_with_1_params(object, ce, &ce->__unset, ZEND_UNSET_FUNC_NAME, NULL, member);

	EG(fake_scope) = orig_fake_scope;
}
/* }}} */

static void zend_std_call_issetter(zval *object, zval *member, zval *retval) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zend_class_entry *orig_fake_scope = EG(fake_scope);

	EG(fake_scope) = NULL;

	/* __isset handler is called with one argument:
	      property name

	   it should return whether the property is set or not
	*/

	zend_call_method_with_1_params(object, ce, &ce->__isset, ZEND_ISSET_FUNC_NAME, retval, member);

	EG(fake_scope) = orig_fake_scope;
}
/* }}} */

static zend_always_inline int zend_verify_property_access(zend_property_info *property_info, zend_class_entry *ce) /* {{{ */
{
	zend_class_entry *scope;

	if (property_info->flags & ZEND_ACC_PUBLIC) {
		return 1;
	} else if (property_info->flags & ZEND_ACC_PRIVATE) {
		if (EG(fake_scope)) {
			scope = EG(fake_scope);
		} else {
			scope = zend_get_executed_scope();
		}
		return (ce == scope || property_info->ce == scope);
	} else if (property_info->flags & ZEND_ACC_PROTECTED) {
		if (EG(fake_scope)) {
			scope = EG(fake_scope);
		} else {
			scope = zend_get_executed_scope();
		}
		return zend_check_protected(property_info->ce, scope);
	}
	return 0;
}
/* }}} */

static zend_always_inline zend_bool is_derived_class(zend_class_entry *child_class, zend_class_entry *parent_class) /* {{{ */
{
	child_class = child_class->parent;
	while (child_class) {
		if (child_class == parent_class) {
			return 1;
		}
		child_class = child_class->parent;
	}

	return 0;
}
/* }}} */

static zend_always_inline uintptr_t zend_get_property_offset(zend_class_entry *ce, zend_string *member, int silent, void **cache_slot) /* {{{ */
{
	zval *zv;
	zend_property_info *property_info = NULL;
	uint32_t flags;
	zend_class_entry *scope;

	if (cache_slot && EXPECTED(ce == CACHED_PTR_EX(cache_slot))) {
		return (uintptr_t)CACHED_PTR_EX(cache_slot + 1);
	}

	if (UNEXPECTED(ZSTR_VAL(member)[0] == '\0' && ZSTR_LEN(member) != 0)) {
		if (!silent) {
			zend_throw_error(NULL, "Cannot access property started with '\\0'");
		}
		return ZEND_WRONG_PROPERTY_OFFSET;
	}

	if (UNEXPECTED(zend_hash_num_elements(&ce->properties_info) == 0)) {
		goto exit_dynamic;
	}

	zv = zend_hash_find(&ce->properties_info, member);
	if (EXPECTED(zv != NULL)) {
		property_info = (zend_property_info*)Z_PTR_P(zv);
		flags = property_info->flags;
		if (UNEXPECTED((flags & ZEND_ACC_SHADOW) != 0)) {
			/* if it's a shadow - go to access it's private */
			property_info = NULL;
		} else {
			if (EXPECTED(zend_verify_property_access(property_info, ce) != 0)) {
				if (UNEXPECTED(!(flags & ZEND_ACC_CHANGED))
					|| UNEXPECTED((flags & ZEND_ACC_PRIVATE))) {
					if (UNEXPECTED((flags & ZEND_ACC_STATIC) != 0)) {
						if (!silent) {
							zend_error(E_NOTICE, "Accessing static property %s::$%s as non static", ZSTR_VAL(ce->name), ZSTR_VAL(member));
						}
						return ZEND_DYNAMIC_PROPERTY_OFFSET;
					}
					goto exit;
				}
			} else {
				/* Try to look in the scope instead */
				property_info = ZEND_WRONG_PROPERTY_INFO;
			}
		}
	}

	if (EG(fake_scope)) {
		scope = EG(fake_scope);
	} else {
		scope = zend_get_executed_scope();
	}

	if (scope != ce
		&& scope
		&& is_derived_class(ce, scope)
		&& (zv = zend_hash_find(&scope->properties_info, member)) != NULL
		&& ((zend_property_info*)Z_PTR_P(zv))->flags & ZEND_ACC_PRIVATE) {
		property_info = (zend_property_info*)Z_PTR_P(zv);
		if (UNEXPECTED((property_info->flags & ZEND_ACC_STATIC) != 0)) {
			return ZEND_DYNAMIC_PROPERTY_OFFSET;
		}
	} else if (UNEXPECTED(property_info == NULL)) {
exit_dynamic:
		if (cache_slot) {
			CACHE_POLYMORPHIC_PTR_EX(cache_slot, ce, (void*)ZEND_DYNAMIC_PROPERTY_OFFSET);
		}
		return ZEND_DYNAMIC_PROPERTY_OFFSET;
	} else if (UNEXPECTED(property_info == ZEND_WRONG_PROPERTY_INFO)) {
		/* Information was available, but we were denied access.  Error out. */
		if (!silent) {
			zend_throw_error(NULL, "Cannot access %s property %s::$%s", zend_visibility_string(flags), ZSTR_VAL(ce->name), ZSTR_VAL(member));
		}
		return ZEND_WRONG_PROPERTY_OFFSET;
	}

exit:
	if (cache_slot) {
		CACHE_POLYMORPHIC_PTR_EX(cache_slot, ce, (void*)(uintptr_t)property_info->offset);
	}
	return property_info->offset;
}
/* }}} */

ZEND_API zend_property_info *zend_get_property_info(zend_class_entry *ce, zend_string *member, int silent) /* {{{ */
{
	zval *zv;
	zend_property_info *property_info = NULL;
	uint32_t flags;
	zend_class_entry *scope;

	if (UNEXPECTED(ZSTR_VAL(member)[0] == '\0' && ZSTR_LEN(member) != 0)) {
		if (!silent) {
			zend_throw_error(NULL, "Cannot access property started with '\\0'");
		}
		return ZEND_WRONG_PROPERTY_INFO;
	}

	if (UNEXPECTED(zend_hash_num_elements(&ce->properties_info) == 0)) {
		goto exit_dynamic;
	}

	zv = zend_hash_find(&ce->properties_info, member);
	if (EXPECTED(zv != NULL)) {
		property_info = (zend_property_info*)Z_PTR_P(zv);
		flags = property_info->flags;
		if (UNEXPECTED((flags & ZEND_ACC_SHADOW) != 0)) {
			/* if it's a shadow - go to access it's private */
			property_info = NULL;
		} else {
			if (EXPECTED(zend_verify_property_access(property_info, ce) != 0)) {
				if (UNEXPECTED(!(flags & ZEND_ACC_CHANGED))
					|| UNEXPECTED((flags & ZEND_ACC_PRIVATE))) {
					if (UNEXPECTED((flags & ZEND_ACC_STATIC) != 0)) {
						if (!silent) {
							zend_error(E_NOTICE, "Accessing static property %s::$%s as non static", ZSTR_VAL(ce->name), ZSTR_VAL(member));
						}
					}
					goto exit;
				}
			} else {
				/* Try to look in the scope instead */
				property_info = ZEND_WRONG_PROPERTY_INFO;
			}
		}
	}

	if (EG(fake_scope)) {
		scope = EG(fake_scope);
	} else {
		scope = zend_get_executed_scope();
	}

	if (scope != ce
		&& scope
		&& is_derived_class(ce, scope)
		&& (zv = zend_hash_find(&scope->properties_info, member)) != NULL
		&& ((zend_property_info*)Z_PTR_P(zv))->flags & ZEND_ACC_PRIVATE) {
		property_info = (zend_property_info*)Z_PTR_P(zv);
	} else if (UNEXPECTED(property_info == NULL)) {
exit_dynamic:
		return NULL;
	} else if (UNEXPECTED(property_info == ZEND_WRONG_PROPERTY_INFO)) {
		/* Information was available, but we were denied access.  Error out. */
		if (!silent) {
			zend_throw_error(NULL, "Cannot access %s property %s::$%s", zend_visibility_string(flags), ZSTR_VAL(ce->name), ZSTR_VAL(member));
		}
		return ZEND_WRONG_PROPERTY_INFO;
	}

exit:
	return property_info;
}
/* }}} */

ZEND_API int zend_check_property_access(zend_object *zobj, zend_string *prop_info_name) /* {{{ */
{
	zend_property_info *property_info;
	const char *class_name = NULL;
	const char *prop_name;
	zend_string *member;
	size_t prop_name_len;

	if (ZSTR_VAL(prop_info_name)[0] == 0) {
		zend_unmangle_property_name_ex(prop_info_name, &class_name, &prop_name, &prop_name_len);
		member = zend_string_init(prop_name, prop_name_len, 0);
	} else {
		member = zend_string_copy(prop_info_name);
	}
	property_info = zend_get_property_info(zobj->ce, member, 1);
	zend_string_release(member);
	if (property_info == NULL) {
		/* undefined public property */
		if (class_name && class_name[0] != '*') {
			/* we we're looking for a private prop */
			return FAILURE;
		}
		return SUCCESS;
	} else if (property_info == ZEND_WRONG_PROPERTY_INFO) {
		return FAILURE;
	}
	if (class_name && class_name[0] != '*') {
		if (!(property_info->flags & ZEND_ACC_PRIVATE)) {
			/* we we're looking for a private prop but found a non private one of the same name */
			return FAILURE;
		} else if (strcmp(ZSTR_VAL(prop_info_name)+1, ZSTR_VAL(property_info->name)+1)) {
			/* we we're looking for a private prop but found a private one of the same name but another class */
			return FAILURE;
		}
	}
	return zend_verify_property_access(property_info, zobj->ce) ? SUCCESS : FAILURE;
}
/* }}} */

static void zend_property_guard_dtor(zval *el) /* {{{ */ {
	uint32_t *ptr = (uint32_t*)Z_PTR_P(el);
	if (EXPECTED(!(((zend_uintptr_t)ptr) & 1))) {
		efree_size(ptr, sizeof(uint32_t));
	}
}
/* }}} */

ZEND_API uint32_t *zend_get_property_guard(zend_object *zobj, zend_string *member) /* {{{ */
{
	HashTable *guards;
	zval *zv;
	uint32_t *ptr;

	ZEND_ASSERT(GC_FLAGS(zobj) & IS_OBJ_USE_GUARDS);
	zv = zobj->properties_table + zobj->ce->default_properties_count;
	if (EXPECTED(Z_TYPE_P(zv) == IS_STRING)) {
		zend_string *str = Z_STR_P(zv);
		if (EXPECTED(str == member) ||
		     /* hash values are always pred-calculated here */
		    (EXPECTED(ZSTR_H(str) == ZSTR_H(member)) &&
		     EXPECTED(ZSTR_LEN(str) == ZSTR_LEN(member)) &&
		     EXPECTED(memcmp(ZSTR_VAL(str), ZSTR_VAL(member), ZSTR_LEN(member)) == 0))) {
			return &zv->u2.property_guard;
		} else if (EXPECTED(zv->u2.property_guard == 0)) {
			zend_string_release(Z_STR_P(zv));
			ZVAL_STR_COPY(zv, member);
			return &zv->u2.property_guard;
		} else {
			ALLOC_HASHTABLE(guards);
			zend_hash_init(guards, 8, NULL, zend_property_guard_dtor, 0);
			/* mark pointer as "special" using low bit */
			zend_hash_add_new_ptr(guards, str,
				(void*)(((zend_uintptr_t)&zv->u2.property_guard) | 1));
			zend_string_release(Z_STR_P(zv));
			ZVAL_ARR(zv, guards);
		}
	} else if (EXPECTED(Z_TYPE_P(zv) == IS_ARRAY)) {
		guards = Z_ARRVAL_P(zv);
		ZEND_ASSERT(guards != NULL);
		zv = zend_hash_find(guards, member);
		if (zv != NULL) {
			return (uint32_t*)(((zend_uintptr_t)Z_PTR_P(zv)) & ~1);
		}
	} else {
		ZEND_ASSERT(Z_TYPE_P(zv) == IS_UNDEF);
		GC_FLAGS(zobj) |= IS_OBJ_HAS_GUARDS;
		ZVAL_STR_COPY(zv, member);
		zv->u2.property_guard = 0;
		return &zv->u2.property_guard;
	}
	/* we have to allocate uint32_t separately because ht->arData may be reallocated */
	ptr = (uint32_t*)emalloc(sizeof(uint32_t));
	*ptr = 0;
	return (uint32_t*)zend_hash_add_new_ptr(guards, member, ptr);
}
/* }}} */

zval *zend_std_read_property(zval *object, zval *member, int type, void **cache_slot, zval *rv) /* {{{ */
{
	zend_object *zobj;
	zval tmp_member;
	zval *retval;
	uintptr_t property_offset;

	zobj = Z_OBJ_P(object);

	ZVAL_UNDEF(&tmp_member);
	if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
		ZVAL_STR(&tmp_member, zval_get_string(member));
		member = &tmp_member;
		cache_slot = NULL;
	}

#if DEBUG_OBJECT_HANDLERS
	fprintf(stderr, "Read object #%d property: %s\n", Z_OBJ_HANDLE_P(object), Z_STRVAL_P(member));
#endif

	/* make zend_get_property_info silent if we have getter - we may want to use it */
	property_offset = zend_get_property_offset(zobj->ce, Z_STR_P(member), (type == BP_VAR_IS) || (zobj->ce->__get != NULL), cache_slot);

	if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
		retval = OBJ_PROP(zobj, property_offset);
		if (EXPECTED(Z_TYPE_P(retval) != IS_UNDEF)) {
			goto exit;
		}
	} else if (EXPECTED(IS_DYNAMIC_PROPERTY_OFFSET(property_offset))) {
		if (EXPECTED(zobj->properties != NULL)) {
			if (!IS_UNKNOWN_DYNAMIC_PROPERTY_OFFSET(property_offset)) {
				uintptr_t idx = ZEND_DECODE_DYN_PROP_OFFSET(property_offset);

				if (EXPECTED(idx < zobj->properties->nNumUsed * sizeof(Bucket))) {
					Bucket *p = (Bucket*)((char*)zobj->properties->arData + idx);

					if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
				        (EXPECTED(p->key == Z_STR_P(member)) ||
				         (EXPECTED(p->h == ZSTR_H(Z_STR_P(member))) &&
				          EXPECTED(p->key != NULL) &&
				          EXPECTED(ZSTR_LEN(p->key) == Z_STRLEN_P(member)) &&
				          EXPECTED(memcmp(ZSTR_VAL(p->key), Z_STRVAL_P(member), Z_STRLEN_P(member)) == 0)))) {
						retval = &p->val;
						goto exit;
					}
				}
				CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_DYNAMIC_PROPERTY_OFFSET);
			}
			retval = zend_hash_find(zobj->properties, Z_STR_P(member));
			if (EXPECTED(retval)) {
				if (cache_slot) {
					uintptr_t idx = (char*)retval - (char*)zobj->properties->arData;
					CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_ENCODE_DYN_PROP_OFFSET(idx));
				}
				goto exit;
			}
		}
	} else if (UNEXPECTED(EG(exception))) {
		retval = &EG(uninitialized_zval);
		goto exit;
	}

	/* magic isset */
	if ((type == BP_VAR_IS) && zobj->ce->__isset) {
		zval tmp_result;
		uint32_t *guard = zend_get_property_guard(zobj, Z_STR_P(member));

		if (!((*guard) & IN_ISSET)) {
			ZVAL_UNDEF(&tmp_result);

			*guard |= IN_ISSET;
			zend_std_call_issetter(object, member, &tmp_result);
			*guard &= ~IN_ISSET;
	
			if (!zend_is_true(&tmp_result)) {
				retval = &EG(uninitialized_zval);
				zval_ptr_dtor(&tmp_result);
				goto exit;
			}

			zval_ptr_dtor(&tmp_result);
		}
	}

	/* magic get */
	if (zobj->ce->__get) {
		uint32_t *guard = zend_get_property_guard(zobj, Z_STR_P(member));
		if (!((*guard) & IN_GET)) {
			/* have getter - try with it! */
			*guard |= IN_GET; /* prevent circular getting */
			zend_std_call_getter(object, member, rv);
			*guard &= ~IN_GET;

			if (Z_TYPE_P(rv) != IS_UNDEF) {
				retval = rv;
				if (!Z_ISREF_P(rv) &&
				    (type == BP_VAR_W || type == BP_VAR_RW  || type == BP_VAR_UNSET)) {
					SEPARATE_ZVAL(rv);
					if (UNEXPECTED(Z_TYPE_P(rv) != IS_OBJECT)) {
						zend_error(E_NOTICE, "Indirect modification of overloaded property %s::$%s has no effect", ZSTR_VAL(zobj->ce->name), Z_STRVAL_P(member));
					}
				}
			} else {
				retval = &EG(uninitialized_zval);
			}
			goto exit;
		} else {
			if (Z_STRVAL_P(member)[0] == '\0' && Z_STRLEN_P(member) != 0) {
				zend_throw_error(NULL, "Cannot access property started with '\\0'");
				retval = &EG(uninitialized_zval);
				goto exit;
			}
		}
	}
	if ((type != BP_VAR_IS)) {
		zend_error(E_NOTICE,"Undefined property: %s::$%s", ZSTR_VAL(zobj->ce->name), Z_STRVAL_P(member));
	}
	retval = &EG(uninitialized_zval);

exit:
	if (UNEXPECTED(Z_REFCOUNTED(tmp_member))) {
		zval_ptr_dtor(&tmp_member);
	}

	return retval;
}
/* }}} */

ZEND_API void zend_std_write_property(zval *object, zval *member, zval *value, void **cache_slot) /* {{{ */
{
	zend_object *zobj;
	zval tmp_member;
	zval *variable_ptr;
	uintptr_t property_offset;

	zobj = Z_OBJ_P(object);

	ZVAL_UNDEF(&tmp_member);
 	if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
		ZVAL_STR(&tmp_member, zval_get_string(member));
		member = &tmp_member;
		cache_slot = NULL;
	}

	property_offset = zend_get_property_offset(zobj->ce, Z_STR_P(member), (zobj->ce->__set != NULL), cache_slot);

	if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
		variable_ptr = OBJ_PROP(zobj, property_offset);
		if (Z_TYPE_P(variable_ptr) != IS_UNDEF) {
			goto found;
		}
	} else if (EXPECTED(IS_DYNAMIC_PROPERTY_OFFSET(property_offset))) {
		if (EXPECTED(zobj->properties != NULL)) {
			if (UNEXPECTED(GC_REFCOUNT(zobj->properties) > 1)) {
				if (EXPECTED(!(GC_FLAGS(zobj->properties) & IS_ARRAY_IMMUTABLE))) {
					GC_REFCOUNT(zobj->properties)--;
				}
				zobj->properties = zend_array_dup(zobj->properties);
			}
			if ((variable_ptr = zend_hash_find(zobj->properties, Z_STR_P(member))) != NULL) {
found:
				zend_assign_to_variable(variable_ptr, value, IS_CV);
				goto exit;
			}
		}
	} else if (UNEXPECTED(EG(exception))) {
		goto exit;
	}

	/* magic set */
	if (zobj->ce->__set) {
		uint32_t *guard = zend_get_property_guard(zobj, Z_STR_P(member));

	    if (!((*guard) & IN_SET)) {
			(*guard) |= IN_SET; /* prevent circular setting */
			zend_std_call_setter(object, member, value);
			(*guard) &= ~IN_SET;
		} else if (EXPECTED(!IS_WRONG_PROPERTY_OFFSET(property_offset))) {
			goto write_std_property;
		} else {
			if (Z_STRVAL_P(member)[0] == '\0' && Z_STRLEN_P(member) != 0) {
				zend_throw_error(NULL, "Cannot access property started with '\\0'");
				goto exit;
			}
		}
	} else if (EXPECTED(!IS_WRONG_PROPERTY_OFFSET(property_offset))) {
		zval tmp;

write_std_property:
		if (Z_REFCOUNTED_P(value)) {
			if (Z_ISREF_P(value)) {
				/* if we assign referenced variable, we should separate it */
				ZVAL_COPY(&tmp, Z_REFVAL_P(value));
				value = &tmp;
			} else {
				Z_ADDREF_P(value);
			}
		}
		if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
			ZVAL_COPY_VALUE(OBJ_PROP(zobj, property_offset), value);
		} else {
			if (!zobj->properties) {
				rebuild_object_properties(zobj);
			}
			zend_hash_add_new(zobj->properties, Z_STR_P(member), value);
		}
	}

exit:
	if (UNEXPECTED(Z_REFCOUNTED(tmp_member))) {
		zval_ptr_dtor(&tmp_member);
	}
}
/* }}} */

zval *zend_std_read_dimension(zval *object, zval *offset, int type, zval *rv) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zval tmp;

	if (EXPECTED(instanceof_function_ex(ce, zend_ce_arrayaccess, 1) != 0)) {
		if (offset == NULL) {
			/* [] construct */
			ZVAL_NULL(&tmp);
			offset = &tmp;
		} else {
			ZVAL_DEREF(offset);
		}

		if (type == BP_VAR_IS) {
			zend_call_method_with_1_params(object, ce, NULL, "offsetexists", rv, offset);
			if (UNEXPECTED(Z_ISUNDEF_P(rv))) {
				return NULL;
			}
			if (!i_zend_is_true(rv)) {
				zval_ptr_dtor(rv);
				return &EG(uninitialized_zval);
			}
			zval_ptr_dtor(rv);
		}

		zend_call_method_with_1_params(object, ce, NULL, "offsetget", rv, offset);

		if (UNEXPECTED(Z_TYPE_P(rv) == IS_UNDEF)) {
			if (UNEXPECTED(!EG(exception))) {
				zend_throw_error(NULL, "Undefined offset for object of type %s used as array", ZSTR_VAL(ce->name));
			}
			return NULL;
		}
		return rv;
	} else {
		zend_throw_error(NULL, "Cannot use object of type %s as array", ZSTR_VAL(ce->name));
		return NULL;
	}
}
/* }}} */

static void zend_std_write_dimension(zval *object, zval *offset, zval *value) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zval tmp;

	if (EXPECTED(instanceof_function_ex(ce, zend_ce_arrayaccess, 1) != 0)) {
		if (!offset) {
			ZVAL_NULL(&tmp);
			offset = &tmp;
		} else {
			ZVAL_DEREF(offset);
		}
		zend_call_method_with_2_params(object, ce, NULL, "offsetset", NULL, offset, value);
	} else {
		zend_throw_error(NULL, "Cannot use object of type %s as array", ZSTR_VAL(ce->name));
	}
}
/* }}} */

static int zend_std_has_dimension(zval *object, zval *offset, int check_empty) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);
	zval retval;
	int result;

	if (EXPECTED(instanceof_function_ex(ce, zend_ce_arrayaccess, 1) != 0)) {
		ZVAL_DEREF(offset);
		zend_call_method_with_1_params(object, ce, NULL, "offsetexists", &retval, offset);
		if (EXPECTED(Z_TYPE(retval) != IS_UNDEF)) {
			result = i_zend_is_true(&retval);
			zval_ptr_dtor(&retval);
			if (check_empty && result && EXPECTED(!EG(exception))) {
				zend_call_method_with_1_params(object, ce, NULL, "offsetget", &retval, offset);
				if (EXPECTED(Z_TYPE(retval) != IS_UNDEF)) {
					result = i_zend_is_true(&retval);
					zval_ptr_dtor(&retval);
				}
			}
		} else {
			result = 0;
		}
	} else {
		zend_throw_error(NULL, "Cannot use object of type %s as array", ZSTR_VAL(ce->name));
		return 0;
	}
	return result;
}
/* }}} */

static zval *zend_std_get_property_ptr_ptr(zval *object, zval *member, int type, void **cache_slot) /* {{{ */
{
	zend_object *zobj;
	zend_string *name;
	zval *retval = NULL;
	uintptr_t property_offset;

	zobj = Z_OBJ_P(object);
	if (EXPECTED(Z_TYPE_P(member) == IS_STRING)) {
		name = Z_STR_P(member);
	} else {
		name = zval_get_string(member);
	}

#if DEBUG_OBJECT_HANDLERS
	fprintf(stderr, "Ptr object #%d property: %s\n", Z_OBJ_HANDLE_P(object), ZSTR_VAL(name));
#endif

	property_offset = zend_get_property_offset(zobj->ce, name, (zobj->ce->__get != NULL), cache_slot);

	if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
		retval = OBJ_PROP(zobj, property_offset);
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
			if (EXPECTED(!zobj->ce->__get) ||
			    UNEXPECTED((*zend_get_property_guard(zobj, name)) & IN_GET)) {
				ZVAL_NULL(retval);
				/* Notice is thrown after creation of the property, to avoid EG(std_property_info)
				 * being overwritten in an error handler. */
				if (UNEXPECTED(type == BP_VAR_RW || type == BP_VAR_R)) {
					zend_error(E_NOTICE, "Undefined property: %s::$%s", ZSTR_VAL(zobj->ce->name), ZSTR_VAL(name));
				}
			} else {
				/* we do have getter - fail and let it try again with usual get/set */
				retval = NULL;
			}
		}
	} else if (EXPECTED(IS_DYNAMIC_PROPERTY_OFFSET(property_offset))) {
		if (EXPECTED(zobj->properties)) {
			if (UNEXPECTED(GC_REFCOUNT(zobj->properties) > 1)) {
				if (EXPECTED(!(GC_FLAGS(zobj->properties) & IS_ARRAY_IMMUTABLE))) {
					GC_REFCOUNT(zobj->properties)--;
				}
				zobj->properties = zend_array_dup(zobj->properties);
			}
		    if (EXPECTED((retval = zend_hash_find(zobj->properties, name)) != NULL)) {
				if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
					zend_string_release(name);
				}
				return retval;
		    }
		}
		if (EXPECTED(!zobj->ce->__get) ||
		    UNEXPECTED((*zend_get_property_guard(zobj, name)) & IN_GET)) {
			if (UNEXPECTED(!zobj->properties)) {
				rebuild_object_properties(zobj);
			}
			retval = zend_hash_update(zobj->properties, name, &EG(uninitialized_zval));
			/* Notice is thrown after creation of the property, to avoid EG(std_property_info)
			 * being overwritten in an error handler. */
			if (UNEXPECTED(type == BP_VAR_RW || type == BP_VAR_R)) {
				zend_error(E_NOTICE, "Undefined property: %s::$%s", ZSTR_VAL(zobj->ce->name), ZSTR_VAL(name));
			}
		}
	}

	if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
		zend_string_release(name);
	}
	return retval;
}
/* }}} */

static void zend_std_unset_property(zval *object, zval *member, void **cache_slot) /* {{{ */
{
	zend_object *zobj;
	zval tmp_member;
	uintptr_t property_offset;

	zobj = Z_OBJ_P(object);

	ZVAL_UNDEF(&tmp_member);
 	if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
		ZVAL_STR(&tmp_member, zval_get_string(member));
		member = &tmp_member;
		cache_slot = NULL;
	}

	property_offset = zend_get_property_offset(zobj->ce, Z_STR_P(member), (zobj->ce->__unset != NULL), cache_slot);

	if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
		zval *slot = OBJ_PROP(zobj, property_offset);

		if (Z_TYPE_P(slot) != IS_UNDEF) {
			zval_ptr_dtor(slot);
			ZVAL_UNDEF(slot);
			if (zobj->properties) {
				zobj->properties->u.v.flags |= HASH_FLAG_HAS_EMPTY_IND;
			}
			goto exit;
		}
	} else if (EXPECTED(IS_DYNAMIC_PROPERTY_OFFSET(property_offset))
	 && EXPECTED(zobj->properties != NULL)) {
		if (UNEXPECTED(GC_REFCOUNT(zobj->properties) > 1)) {
			if (EXPECTED(!(GC_FLAGS(zobj->properties) & IS_ARRAY_IMMUTABLE))) {
				GC_REFCOUNT(zobj->properties)--;
			}
			zobj->properties = zend_array_dup(zobj->properties);
		}
		if (EXPECTED(zend_hash_del(zobj->properties, Z_STR_P(member)) != FAILURE)) {
			goto exit;
		}
	} else if (UNEXPECTED(EG(exception))) {
		goto exit;
	}

	/* magic unset */
	if (zobj->ce->__unset) {
		uint32_t *guard = zend_get_property_guard(zobj, Z_STR_P(member));
		if (!((*guard) & IN_UNSET)) {
			/* have unseter - try with it! */
			(*guard) |= IN_UNSET; /* prevent circular unsetting */
			zend_std_call_unsetter(object, member);
			(*guard) &= ~IN_UNSET;
		} else {
			if (Z_STRVAL_P(member)[0] == '\0' && Z_STRLEN_P(member) != 0) {
				zend_throw_error(NULL, "Cannot access property started with '\\0'");
				goto exit;
			}
		}
	}

exit:
	if (UNEXPECTED(Z_REFCOUNTED(tmp_member))) {
		zval_ptr_dtor(&tmp_member);
	}
}
/* }}} */

static void zend_std_unset_dimension(zval *object, zval *offset) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(object);

	if (instanceof_function_ex(ce, zend_ce_arrayaccess, 1)) {
		ZVAL_DEREF(offset);
		zend_call_method_with_1_params(object, ce, NULL, "offsetunset", NULL, offset);
	} else {
		zend_throw_error(NULL, "Cannot use object of type %s as array", ZSTR_VAL(ce->name));
	}
}
/* }}} */

/* Ensures that we're allowed to call a private method.
 * Returns the function address that should be called, or NULL
 * if no such function exists.
 */
static inline zend_function *zend_check_private_int(zend_function *fbc, zend_class_entry *ce, zend_string *function_name) /* {{{ */
{
    zval *func;
    zend_class_entry *scope;

	if (!ce) {
		return 0;
	}

	/* We may call a private function if:
	 * 1.  The class of our object is the same as the scope, and the private
	 *     function (EX(fbc)) has the same scope.
	 * 2.  One of our parent classes are the same as the scope, and it contains
	 *     a private function with the same name that has the same scope.
	 */
	scope = zend_get_executed_scope();
	if (fbc->common.scope == ce && scope == ce) {
		/* rule #1 checks out ok, allow the function call */
		return fbc;
	}


	/* Check rule #2 */
	ce = ce->parent;
	while (ce) {
		if (ce == scope) {
			if ((func = zend_hash_find(&ce->function_table, function_name))) {
				fbc = Z_FUNC_P(func);
				if (fbc->common.fn_flags & ZEND_ACC_PRIVATE
					&& fbc->common.scope == scope) {
					return fbc;
				}
			}
			break;
		}
		ce = ce->parent;
	}
	return NULL;
}
/* }}} */

ZEND_API int zend_check_private(zend_function *fbc, zend_class_entry *ce, zend_string *function_name) /* {{{ */
{
	return zend_check_private_int(fbc, ce, function_name) != NULL;
}
/* }}} */

/* Ensures that we're allowed to call a protected method.
 */
ZEND_API int zend_check_protected(zend_class_entry *ce, zend_class_entry *scope) /* {{{ */
{
	zend_class_entry *fbc_scope = ce;

	/* Is the context that's calling the function, the same as one of
	 * the function's parents?
	 */
	while (fbc_scope) {
		if (fbc_scope==scope) {
			return 1;
		}
		fbc_scope = fbc_scope->parent;
	}

	/* Is the function's scope the same as our current object context,
	 * or any of the parents of our context?
	 */
	while (scope) {
		if (scope==ce) {
			return 1;
		}
		scope = scope->parent;
	}
	return 0;
}
/* }}} */

ZEND_API zend_function *zend_get_call_trampoline_func(zend_class_entry *ce, zend_string *method_name, int is_static) /* {{{ */
{
	size_t mname_len;
	zend_op_array *func;
	zend_function *fbc = is_static ? ce->__callstatic : ce->__call;

	ZEND_ASSERT(fbc);

	if (EXPECTED(EG(trampoline).common.function_name == NULL)) {
		func = &EG(trampoline).op_array;
	} else {
		func = ecalloc(1, sizeof(zend_op_array));
	}

	func->type = ZEND_USER_FUNCTION;
	func->arg_flags[0] = 0;
	func->arg_flags[1] = 0;
	func->arg_flags[2] = 0;
	func->fn_flags = ZEND_ACC_CALL_VIA_TRAMPOLINE | ZEND_ACC_PUBLIC;
	if (is_static) {
		func->fn_flags |= ZEND_ACC_STATIC;
	}
	func->opcodes = &EG(call_trampoline_op);

	func->prototype = fbc;
	func->scope = fbc->common.scope;
	/* reserve space for arguments, local and temorary variables */
	func->T = (fbc->type == ZEND_USER_FUNCTION)? MAX(fbc->op_array.last_var + fbc->op_array.T, 2) : 2;
	func->filename = (fbc->type == ZEND_USER_FUNCTION)? fbc->op_array.filename : ZSTR_EMPTY_ALLOC();
	func->line_start = (fbc->type == ZEND_USER_FUNCTION)? fbc->op_array.line_start : 0;
	func->line_end = (fbc->type == ZEND_USER_FUNCTION)? fbc->op_array.line_end : 0;

	//??? keep compatibility for "\0" characters
	//??? see: Zend/tests/bug46238.phpt
	if (UNEXPECTED((mname_len = strlen(ZSTR_VAL(method_name))) != ZSTR_LEN(method_name))) {
		func->function_name = zend_string_init(ZSTR_VAL(method_name), mname_len, 0);
	} else {
		func->function_name = zend_string_copy(method_name);
	}

	return (zend_function*)func;
}
/* }}} */

static zend_always_inline zend_function *zend_get_user_call_function(zend_class_entry *ce, zend_string *method_name) /* {{{ */
{
	return zend_get_call_trampoline_func(ce, method_name, 0);
}
/* }}} */

static union _zend_function *zend_std_get_method(zend_object **obj_ptr, zend_string *method_name, const zval *key) /* {{{ */
{
	zend_object *zobj = *obj_ptr;
	zval *func;
	zend_function *fbc;
	zend_string *lc_method_name;
	zend_class_entry *scope = NULL;
	ALLOCA_FLAG(use_heap);

	if (EXPECTED(key != NULL)) {
		lc_method_name = Z_STR_P(key);
#ifdef ZEND_ALLOCA_MAX_SIZE
		use_heap = 0;
#endif
	} else {
		ZSTR_ALLOCA_ALLOC(lc_method_name, ZSTR_LEN(method_name), use_heap);
		zend_str_tolower_copy(ZSTR_VAL(lc_method_name), ZSTR_VAL(method_name), ZSTR_LEN(method_name));
	}

	if (UNEXPECTED((func = zend_hash_find(&zobj->ce->function_table, lc_method_name)) == NULL)) {
		if (UNEXPECTED(!key)) {
			ZSTR_ALLOCA_FREE(lc_method_name, use_heap);
		}
		if (zobj->ce->__call) {
			return zend_get_user_call_function(zobj->ce, method_name);
		} else {
			return NULL;
		}
	}

	fbc = Z_FUNC_P(func);
	/* Check access level */
	if (fbc->op_array.fn_flags & ZEND_ACC_PRIVATE) {
		zend_function *updated_fbc;

		/* Ensure that if we're calling a private function, we're allowed to do so.
		 * If we're not and __call() handler exists, invoke it, otherwise error out.
		 */
		updated_fbc = zend_check_private_int(fbc, zobj->ce, lc_method_name);
		if (EXPECTED(updated_fbc != NULL)) {
			fbc = updated_fbc;
		} else {
			if (zobj->ce->__call) {
				fbc = zend_get_user_call_function(zobj->ce, method_name);
			} else {
				scope = zend_get_executed_scope();
				zend_throw_error(NULL, "Call to %s method %s::%s() from context '%s'", zend_visibility_string(fbc->common.fn_flags), ZEND_FN_SCOPE_NAME(fbc), ZSTR_VAL(method_name), scope ? ZSTR_VAL(scope->name) : "");
				fbc = NULL;
			}
		}
	} else {
		/* Ensure that we haven't overridden a private function and end up calling
		 * the overriding public function...
		 */
		if (fbc->op_array.fn_flags & (ZEND_ACC_CHANGED|ZEND_ACC_PROTECTED)) {
			scope = zend_get_executed_scope();
		}
		if (fbc->op_array.fn_flags & ZEND_ACC_CHANGED) {
			if (scope && is_derived_class(fbc->common.scope, scope)) {
				if ((func = zend_hash_find(&scope->function_table, lc_method_name)) != NULL) {
					zend_function *priv_fbc = Z_FUNC_P(func);
					if (priv_fbc->common.fn_flags & ZEND_ACC_PRIVATE
						&& priv_fbc->common.scope == scope) {
						fbc = priv_fbc;
					}
				}
			}
		}
		if (fbc->common.fn_flags & ZEND_ACC_PROTECTED) {
			/* Ensure that if we're calling a protected function, we're allowed to do so.
			 * If we're not and __call() handler exists, invoke it, otherwise error out.
			 */
			if (UNEXPECTED(!zend_check_protected(zend_get_function_root_class(fbc), scope))) {
				if (zobj->ce->__call) {
					fbc = zend_get_user_call_function(zobj->ce, method_name);
				} else {
					zend_throw_error(NULL, "Call to %s method %s::%s() from context '%s'", zend_visibility_string(fbc->common.fn_flags), ZEND_FN_SCOPE_NAME(fbc), ZSTR_VAL(method_name), scope ? ZSTR_VAL(scope->name) : "");
					fbc = NULL;
				}
			}
		}
	}

	if (UNEXPECTED(!key)) {
		ZSTR_ALLOCA_FREE(lc_method_name, use_heap);
	}
	return fbc;
}
/* }}} */

static zend_always_inline zend_function *zend_get_user_callstatic_function(zend_class_entry *ce, zend_string *method_name) /* {{{ */
{
	return zend_get_call_trampoline_func(ce, method_name, 1);
}
/* }}} */

ZEND_API zend_function *zend_std_get_static_method(zend_class_entry *ce, zend_string *function_name, const zval *key) /* {{{ */
{
	zend_function *fbc = NULL;
	zend_string *lc_function_name;
	zend_object *object;
	zend_class_entry *scope;

	if (EXPECTED(key != NULL)) {
		lc_function_name = Z_STR_P(key);
	} else {
		lc_function_name = zend_string_tolower(function_name);
	}

	do {
		zval *func = zend_hash_find(&ce->function_table, lc_function_name);
		if (EXPECTED(func != NULL)) {
			fbc = Z_FUNC_P(func);
		} else if (ce->constructor
			&& ZSTR_LEN(lc_function_name) == ZSTR_LEN(ce->name)
			&& zend_binary_strncasecmp(ZSTR_VAL(lc_function_name), ZSTR_LEN(lc_function_name), ZSTR_VAL(ce->name), ZSTR_LEN(lc_function_name), ZSTR_LEN(lc_function_name)) == 0
			/* Only change the method to the constructor if the constructor isn't called __construct
			 * we check for __ so we can be binary safe for lowering, we should use ZEND_CONSTRUCTOR_FUNC_NAME
			 */
			&& (ZSTR_VAL(ce->constructor->common.function_name)[0] != '_'
				|| ZSTR_VAL(ce->constructor->common.function_name)[1] != '_')) {
			fbc = ce->constructor;
		} else {
			if (UNEXPECTED(!key)) {
				zend_string_release(lc_function_name);
			}
			if (ce->__call &&
				(object = zend_get_this_object(EG(current_execute_data))) != NULL &&
			    instanceof_function(object->ce, ce)) {
				/* Call the top-level defined __call().
				 * see: tests/classes/__call_004.phpt  */

				zend_class_entry *call_ce = object->ce;

				while (!call_ce->__call) {
					call_ce = call_ce->parent;
				}
				return zend_get_user_call_function(call_ce, function_name);
			} else if (ce->__callstatic) {
				return zend_get_user_callstatic_function(ce, function_name);
			} else {
	   			return NULL;
			}
		}
	} while (0);

#if MBO_0
	/* right now this function is used for non static method lookup too */
	/* Is the function static */
	if (UNEXPECTED(!(fbc->common.fn_flags & ZEND_ACC_STATIC))) {
		zend_error_noreturn(E_ERROR, "Cannot call non static method %s::%s() without object", ZEND_FN_SCOPE_NAME(fbc), ZSTR_VAL(fbc->common.function_name));
	}
#endif
	if (fbc->op_array.fn_flags & ZEND_ACC_PUBLIC) {
		/* No further checks necessary, most common case */
	} else if (fbc->op_array.fn_flags & ZEND_ACC_PRIVATE) {
		zend_function *updated_fbc;

		/* Ensure that if we're calling a private function, we're allowed to do so.
		 */
		scope = zend_get_executed_scope();
		updated_fbc = zend_check_private_int(fbc, scope, lc_function_name);
		if (EXPECTED(updated_fbc != NULL)) {
			fbc = updated_fbc;
		} else {
			if (ce->__callstatic) {
				fbc = zend_get_user_callstatic_function(ce, function_name);
			} else {
				zend_throw_error(NULL, "Call to %s method %s::%s() from context '%s'", zend_visibility_string(fbc->common.fn_flags), ZEND_FN_SCOPE_NAME(fbc), ZSTR_VAL(function_name), scope ? ZSTR_VAL(scope->name) : "");
				fbc = NULL;
			}
		}
	} else if ((fbc->common.fn_flags & ZEND_ACC_PROTECTED)) {
		/* Ensure that if we're calling a protected function, we're allowed to do so.
		 */
		scope = zend_get_executed_scope();
		if (UNEXPECTED(!zend_check_protected(zend_get_function_root_class(fbc), scope))) {
			if (ce->__callstatic) {
				fbc = zend_get_user_callstatic_function(ce, function_name);
			} else {
				zend_throw_error(NULL, "Call to %s method %s::%s() from context '%s'", zend_visibility_string(fbc->common.fn_flags), ZEND_FN_SCOPE_NAME(fbc), ZSTR_VAL(function_name), scope ? ZSTR_VAL(scope->name) : "");
				fbc = NULL;
			}
		}
	}

	if (UNEXPECTED(!key)) {
		zend_string_release(lc_function_name);
	}

	return fbc;
}
/* }}} */

ZEND_API zval *zend_std_get_static_property(zend_class_entry *ce, zend_string *property_name, zend_bool silent) /* {{{ */
{
	zend_property_info *property_info = zend_hash_find_ptr(&ce->properties_info, property_name);
	zval *ret;

	if (UNEXPECTED(property_info == NULL)) {
		goto undeclared_property;
	}

	if (UNEXPECTED(!zend_verify_property_access(property_info, ce))) {
		if (!silent) {
			zend_throw_error(NULL, "Cannot access %s property %s::$%s", zend_visibility_string(property_info->flags), ZSTR_VAL(ce->name), ZSTR_VAL(property_name));
		}
		return NULL;
	}

	if (UNEXPECTED((property_info->flags & ZEND_ACC_STATIC) == 0)) {
		goto undeclared_property;
	}

	if (UNEXPECTED(!(ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED))) {
		if (UNEXPECTED(zend_update_class_constants(ce)) != SUCCESS) {
			return NULL;
		}
	}
	ret = CE_STATIC_MEMBERS(ce) + property_info->offset;

	/* check if static properties were destoyed */
	if (UNEXPECTED(CE_STATIC_MEMBERS(ce) == NULL)) {
undeclared_property:
		if (!silent) {
			zend_throw_error(NULL, "Access to undeclared static property: %s::$%s", ZSTR_VAL(ce->name), ZSTR_VAL(property_name));
		}
		ret = NULL;
	}

	return ret;
}
/* }}} */

ZEND_API ZEND_COLD zend_bool zend_std_unset_static_property(zend_class_entry *ce, zend_string *property_name) /* {{{ */
{
	zend_throw_error(NULL, "Attempt to unset static property %s::$%s", ZSTR_VAL(ce->name), ZSTR_VAL(property_name));
	return 0;
}
/* }}} */

ZEND_API union _zend_function *zend_std_get_constructor(zend_object *zobj) /* {{{ */
{
	zend_function *constructor = zobj->ce->constructor;
	zend_class_entry *scope;

	if (constructor) {
		if (constructor->op_array.fn_flags & ZEND_ACC_PUBLIC) {
			/* No further checks necessary */
		} else if (constructor->op_array.fn_flags & ZEND_ACC_PRIVATE) {
			/* Ensure that if we're calling a private function, we're allowed to do so.
			 */
			if (EG(fake_scope)) {
				scope = EG(fake_scope);
			} else {
				scope = zend_get_executed_scope();
			}
			if (UNEXPECTED(constructor->common.scope != scope)) {
				if (scope) {
					zend_throw_error(NULL, "Call to private %s::%s() from context '%s'", ZSTR_VAL(constructor->common.scope->name), ZSTR_VAL(constructor->common.function_name), ZSTR_VAL(scope->name));
					constructor = NULL;
				} else {
					zend_throw_error(NULL, "Call to private %s::%s() from invalid context", ZSTR_VAL(constructor->common.scope->name), ZSTR_VAL(constructor->common.function_name));
					constructor = NULL;
				}
			}
		} else if ((constructor->common.fn_flags & ZEND_ACC_PROTECTED)) {
			/* Ensure that if we're calling a protected function, we're allowed to do so.
			 * Constructors only have prototype if they are defined by an interface but
			 * it is the compilers responsibility to take care of the prototype.
			 */
			if (EG(fake_scope)) {
				scope = EG(fake_scope);
			} else {
				scope = zend_get_executed_scope();
			}
			if (UNEXPECTED(!zend_check_protected(zend_get_function_root_class(constructor), scope))) {
				if (scope) {
					zend_throw_error(NULL, "Call to protected %s::%s() from context '%s'", ZSTR_VAL(constructor->common.scope->name), ZSTR_VAL(constructor->common.function_name), ZSTR_VAL(scope->name));
					constructor = NULL;
				} else {
					zend_throw_error(NULL, "Call to protected %s::%s() from invalid context", ZSTR_VAL(constructor->common.scope->name), ZSTR_VAL(constructor->common.function_name));
					constructor = NULL;
				}
			}
		}
	}

	return constructor;
}
/* }}} */

static int zend_std_compare_objects(zval *o1, zval *o2) /* {{{ */
{
	zend_object *zobj1, *zobj2;

	zobj1 = Z_OBJ_P(o1);
	zobj2 = Z_OBJ_P(o2);

	if (zobj1 == zobj2) {
		return 0; /* the same object */
	}
	if (zobj1->ce != zobj2->ce) {
		return 1; /* different classes */
	}
	if (!zobj1->properties && !zobj2->properties) {
		zval *p1, *p2, *end;

		if (!zobj1->ce->default_properties_count) {
			return 0;
		}
		p1 = zobj1->properties_table;
		p2 = zobj2->properties_table;
		end = p1 + zobj1->ce->default_properties_count;

		/* It's enough to protect only one of the objects.
		 * The second one may be referenced from the first and this may cause
		 * false recursion detection.
		 */
		/* use bitwise OR to make only one conditional jump */
		if (UNEXPECTED(Z_IS_RECURSIVE_P(o1))) {
			zend_error_noreturn(E_ERROR, "Nesting level too deep - recursive dependency?");
		}
		Z_PROTECT_RECURSION_P(o1);
		do {
			if (Z_TYPE_P(p1) != IS_UNDEF) {
				if (Z_TYPE_P(p2) != IS_UNDEF) {
					zval result;

					if (compare_function(&result, p1, p2)==FAILURE) {
						Z_UNPROTECT_RECURSION_P(o1);
						return 1;
					}
					if (Z_LVAL(result) != 0) {
						Z_UNPROTECT_RECURSION_P(o1);
						return Z_LVAL(result);
					}
				} else {
					Z_UNPROTECT_RECURSION_P(o1);
					return 1;
				}
			} else {
				if (Z_TYPE_P(p2) != IS_UNDEF) {
					Z_UNPROTECT_RECURSION_P(o1);
					return 1;
				}
			}
			p1++;
			p2++;
		} while (p1 != end);
		Z_UNPROTECT_RECURSION_P(o1);
		return 0;
	} else {
		if (!zobj1->properties) {
			rebuild_object_properties(zobj1);
		}
		if (!zobj2->properties) {
			rebuild_object_properties(zobj2);
		}
		return zend_compare_symbol_tables(zobj1->properties, zobj2->properties);
	}
}
/* }}} */

static int zend_std_has_property(zval *object, zval *member, int has_set_exists, void **cache_slot) /* {{{ */
{
	zend_object *zobj;
	int result;
	zval *value = NULL;
	zval tmp_member;
	uintptr_t property_offset;

	zobj = Z_OBJ_P(object);

	ZVAL_UNDEF(&tmp_member);
	if (UNEXPECTED(Z_TYPE_P(member) != IS_STRING)) {
		ZVAL_STR(&tmp_member, zval_get_string(member));
		member = &tmp_member;
		cache_slot = NULL;
	}

	property_offset = zend_get_property_offset(zobj->ce, Z_STR_P(member), 1, cache_slot);

	if (EXPECTED(IS_VALID_PROPERTY_OFFSET(property_offset))) {
		value = OBJ_PROP(zobj, property_offset);
		if (Z_TYPE_P(value) != IS_UNDEF) {
			goto found;
		}
	} else if (EXPECTED(IS_DYNAMIC_PROPERTY_OFFSET(property_offset))) {
		if (EXPECTED(zobj->properties != NULL)) {
			if (!IS_UNKNOWN_DYNAMIC_PROPERTY_OFFSET(property_offset)) {
				uintptr_t idx = ZEND_DECODE_DYN_PROP_OFFSET(property_offset);

				if (EXPECTED(idx < zobj->properties->nNumUsed * sizeof(Bucket))) {
					Bucket *p = (Bucket*)((char*)zobj->properties->arData + idx);

					if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
				        (EXPECTED(p->key == Z_STR_P(member)) ||
				         (EXPECTED(p->h == ZSTR_H(Z_STR_P(member))) &&
				          EXPECTED(p->key != NULL) &&
				          EXPECTED(ZSTR_LEN(p->key) == Z_STRLEN_P(member)) &&
				          EXPECTED(memcmp(ZSTR_VAL(p->key), Z_STRVAL_P(member), Z_STRLEN_P(member)) == 0)))) {
						value = &p->val;
						goto found;
					}
				}
				CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_DYNAMIC_PROPERTY_OFFSET);
			}
			value = zend_hash_find(zobj->properties, Z_STR_P(member));
			if (value) {
				if (cache_slot) {
					uintptr_t idx = (char*)value - (char*)zobj->properties->arData;
					CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_ENCODE_DYN_PROP_OFFSET(idx));
				}
found:
				switch (has_set_exists) {
					case 0:
						ZVAL_DEREF(value);
						result = (Z_TYPE_P(value) != IS_NULL);
						break;
					default:
						result = zend_is_true(value);
						break;
					case 2:
						result = 1;
						break;
				}
				goto exit;
			}
		}
	} else if (UNEXPECTED(EG(exception))) {
		result = 0;
		goto exit;
	}

	result = 0;
	if ((has_set_exists != 2) && zobj->ce->__isset) {
		uint32_t *guard = zend_get_property_guard(zobj, Z_STR_P(member));

		if (!((*guard) & IN_ISSET)) {
			zval rv;

			/* have issetter - try with it! */
			(*guard) |= IN_ISSET; /* prevent circular getting */
			zend_std_call_issetter(object, member, &rv);
			if (Z_TYPE(rv) != IS_UNDEF) {
				result = zend_is_true(&rv);
				zval_ptr_dtor(&rv);
				if (has_set_exists && result) {
					if (EXPECTED(!EG(exception)) && zobj->ce->__get && !((*guard) & IN_GET)) {
						(*guard) |= IN_GET;
						zend_std_call_getter(object, member, &rv);
						(*guard) &= ~IN_GET;
						if (Z_TYPE(rv) != IS_UNDEF) {
							result = i_zend_is_true(&rv);
							zval_ptr_dtor(&rv);
						} else {
							result = 0;
						}
					} else {
						result = 0;
					}
				}
			}
			(*guard) &= ~IN_ISSET;
		}
	}

exit:
	if (UNEXPECTED(Z_REFCOUNTED(tmp_member))) {
		zval_ptr_dtor(&tmp_member);
	}
	return result;
}
/* }}} */

zend_string *zend_std_object_get_class_name(const zend_object *zobj) /* {{{ */
{
	return zend_string_copy(zobj->ce->name);
}
/* }}} */

ZEND_API int zend_std_cast_object_tostring(zval *readobj, zval *writeobj, int type) /* {{{ */
{
	zval retval;
	zend_class_entry *ce;

	switch (type) {
		case IS_STRING:
			ce = Z_OBJCE_P(readobj);
			if (ce->__tostring &&
				(zend_call_method_with_0_params(readobj, ce, &ce->__tostring, "__tostring", &retval) || EG(exception))) {
				if (UNEXPECTED(EG(exception) != NULL)) {
					zval *msg, ex, rv;
					zval_ptr_dtor(&retval);
					ZVAL_OBJ(&ex, EG(exception));
					EG(exception) = NULL;
					msg = zend_read_property(Z_OBJCE(ex), &ex, "message", sizeof("message") - 1, 1, &rv);
					if (UNEXPECTED(Z_TYPE_P(msg) != IS_STRING)) {
						ZVAL_EMPTY_STRING(&rv);
						msg = &rv;
					}
					zend_error_noreturn(E_ERROR,
							"Method %s::__toString() must not throw an exception, caught %s: %s",
							ZSTR_VAL(ce->name), ZSTR_VAL(Z_OBJCE(ex)->name), Z_STRVAL_P(msg));
					return FAILURE;
				}
				if (EXPECTED(Z_TYPE(retval) == IS_STRING)) {
					if (readobj == writeobj) {
						zval_ptr_dtor(readobj);
					}
					ZVAL_COPY_VALUE(writeobj, &retval);
					return SUCCESS;
				} else {
					zval_ptr_dtor(&retval);
					if (readobj == writeobj) {
						zval_ptr_dtor(readobj);
					}
					ZVAL_EMPTY_STRING(writeobj);
					zend_error(E_RECOVERABLE_ERROR, "Method %s::__toString() must return a string value", ZSTR_VAL(ce->name));
					return SUCCESS;
				}
			}
			return FAILURE;
		case _IS_BOOL:
			ZVAL_BOOL(writeobj, 1);
			return SUCCESS;
		case IS_LONG:
			ce = Z_OBJCE_P(readobj);
			zend_error(E_NOTICE, "Object of class %s could not be converted to int", ZSTR_VAL(ce->name));
			if (readobj == writeobj) {
				zval_dtor(readobj);
			}
			ZVAL_LONG(writeobj, 1);
			return SUCCESS;
		case IS_DOUBLE:
			ce = Z_OBJCE_P(readobj);
			zend_error(E_NOTICE, "Object of class %s could not be converted to float", ZSTR_VAL(ce->name));
			if (readobj == writeobj) {
				zval_dtor(readobj);
			}
			ZVAL_DOUBLE(writeobj, 1);
			return SUCCESS;
		default:
			ZVAL_NULL(writeobj);
			break;
	}
	return FAILURE;
}
/* }}} */

int zend_std_get_closure(zval *obj, zend_class_entry **ce_ptr, zend_function **fptr_ptr, zend_object **obj_ptr) /* {{{ */
{
	zval *func;
	zend_class_entry *ce;

	if (Z_TYPE_P(obj) != IS_OBJECT) {
		return FAILURE;
	}

	ce = Z_OBJCE_P(obj);

	if ((func = zend_hash_find(&ce->function_table, ZSTR_KNOWN(ZEND_STR_MAGIC_INVOKE))) == NULL) {
		return FAILURE;
	}
	*fptr_ptr = Z_FUNC_P(func);

	*ce_ptr = ce;
	if ((*fptr_ptr)->common.fn_flags & ZEND_ACC_STATIC) {
		if (obj_ptr) {
			*obj_ptr = NULL;
		}
	} else {
		if (obj_ptr) {
			*obj_ptr = Z_OBJ_P(obj);
		}
	}
	return SUCCESS;
}
/* }}} */

ZEND_API zend_object_handlers std_object_handlers = {
	0,										/* offset */

	zend_object_std_dtor,					/* free_obj */
	zend_objects_destroy_object,			/* dtor_obj */
	zend_objects_clone_obj,					/* clone_obj */

	zend_std_read_property,					/* read_property */
	zend_std_write_property,				/* write_property */
	zend_std_read_dimension,				/* read_dimension */
	zend_std_write_dimension,				/* write_dimension */
	zend_std_get_property_ptr_ptr,			/* get_property_ptr_ptr */
	NULL,									/* get */
	NULL,									/* set */
	zend_std_has_property,					/* has_property */
	zend_std_unset_property,				/* unset_property */
	zend_std_has_dimension,					/* has_dimension */
	zend_std_unset_dimension,				/* unset_dimension */
	zend_std_get_properties,				/* get_properties */
	zend_std_get_method,					/* get_method */
	NULL,									/* call_method */
	zend_std_get_constructor,				/* get_constructor */
	zend_std_object_get_class_name,			/* get_class_name */
	zend_std_compare_objects,				/* compare_objects */
	zend_std_cast_object_tostring,			/* cast_object */
	NULL,									/* count_elements */
	zend_std_get_debug_info,				/* get_debug_info */
	zend_std_get_closure,					/* get_closure */
	zend_std_get_gc,						/* get_gc */
	NULL,									/* do_operation */
	NULL,									/* compare */
};

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
