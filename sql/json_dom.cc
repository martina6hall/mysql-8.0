/* Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "json_dom.h"

#include <cmath>                // std::isfinite
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <algorithm>            // std::min, std::max
#include <memory>               // std::unique_ptr

#include "base64.h"
#include "check_stack.h"
#include "current_thd.h"        // current_thd
#include "decimal.h"
#include "derror.h"             // ER_THD
#include "field.h"
#include "json_diff.h"
#include "json_path.h"
#include "m_ctype.h"
#include "m_string.h"           // my_gcvt, _dig_vec_lower, my_strtod
#include "my_byteorder.h"
#include "my_dbug.h"
#include "my_double2ulonglong.h"
#include "my_rapidjson_size_t.h"
#include "my_sys.h"
#include "my_time.h"
#include "mysql/psi/mysql_statement.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql_com.h"
#include "mysqld_error.h"       // ER_*
#include "psi_memory_key.h"     // key_memory_JSON
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/memorystream.h"
#include "rapidjson/reader.h"
#include "sql_class.h"          // THD
#include "sql_const.h"          // STACK_MIN_SIZE
#include "sql_error.h"
#include "sql_string.h"
#include "sql_time.h"
#include "system_variables.h"
#include "template_utils.h"     // down_cast, pointer_cast

using namespace rapidjson;

static Json_dom *json_binary_to_dom_template(const json_binary::Value &v);
static bool populate_object_or_array(const THD *thd, Json_dom *dom,
                                     const json_binary::Value &v);
static bool populate_object(const THD *thd, Json_object *jo,
                            const json_binary::Value &v);
static bool populate_array(const THD *thd, Json_array *ja,
                           const json_binary::Value &v);

/**
 Auto-wrap a dom. Delete the dom if there is a memory
 allocation failure.
*/
static Json_array *wrap_in_array(Json_dom *dom)
{
  Json_array *array= new (std::nothrow) Json_array(dom);
  if (array == NULL)
    delete dom;                               /* purecov: inspected */
  return array;
}


/**
  A dom is mergeable if it is an array or an object. All other
  types must be wrapped in an array in order to be merged.
  Delete the candidate if there is a memory allocation failure.
*/
static Json_dom *make_mergeable(Json_dom *candidate)
{
  switch (candidate->json_type())
  {
  case enum_json_type::J_ARRAY:
  case enum_json_type::J_OBJECT:
    {
      return candidate;
    }
  default:
    {
      return wrap_in_array(candidate);
    }
  }
}

Json_dom *merge_doms(Json_dom *left, Json_dom *right)
{
  left= make_mergeable(left);
  if (!left)
  {
    /* purecov: begin inspected */
    delete right;
    return NULL;
    /* purecov: end */
  }

  right= make_mergeable(right);
  if (!right)
  {
    /* purecov: begin inspected */
    delete left;
    return NULL;
    /* purecov: end */
  }

  // at this point, the arguments are either objects or arrays
  bool left_is_array= (left->json_type() == enum_json_type::J_ARRAY);
  bool right_is_array= (right->json_type() == enum_json_type::J_ARRAY);

  if (left_is_array || right_is_array)
  {
    if (!left_is_array)
      left= wrap_in_array(left);
    if (!left)
    {
      /* purecov: begin inspected */
      delete right;
      return NULL;
      /* purecov: end */
    }

    if (!right_is_array)
      right= wrap_in_array(right);
    if (!right)
    {
      /* purecov: begin inspected */
      delete left;
      return NULL;
      /* purecov: end */
    }

    if (down_cast<Json_array *>(left)->consume(down_cast<Json_array *>(right)))
    {
      delete left;                              /* purecov: inspected */
      return NULL;                              /* purecov: inspected */
    }
  }
  else  // otherwise, both doms are objects
  {
    if (down_cast<Json_object *>(left)
        ->consume(down_cast<Json_object *>(right)))
    {
      delete left;                              /* purecov: inspected */
      return NULL;                              /* purecov: inspected */
    }
  }

  return left;
}


void *Json_dom::operator new(size_t size, const std::nothrow_t&) throw()
{
  /*
    Call my_malloc() with the MY_WME flag to make sure that it will
    write an error message if the memory could not be allocated.
  */
  return my_malloc(key_memory_JSON, size, MYF(MY_WME));
}


void Json_dom::operator delete(void *ptr) throw()
{
  my_free(ptr);
}


/*
  This operator is included in order to silence warnings on some
  compilers. It is called if the constructor throws an exception when
  an object is allocated with nothrow new. This is not supposed to
  happen and is therefore hard to test, so annotate it to avoid
  cluttering the test coverage reports.
*/
/* purecov: begin inspected */
void Json_dom::operator delete(void *ptr, const std::nothrow_t&) throw()
{
  operator delete(ptr);
}
/* purecov: end */


static bool seen_already(Json_dom_vector *result, Json_dom *cand)
{
  Json_dom_vector::iterator it= std::find(result->begin(),
                                          result->end(),
                                          cand);
  return it != result->end();
}

/**
  Add a value to a vector if it isn't already there.

  @param[in] candidate value to add
  @param[in,out] duplicates set of values added
  @param[in,out] result vector
  @return false on success, true on error
*/
static bool add_if_missing(Json_dom *candidate,
                           Json_dom_vector *duplicates,
                           Json_dom_vector *result)
{
  if (duplicates->insert_unique(candidate).second)
  {
    return result->push_back(candidate);
  }
  return false;
}


/**
  Check if a seek operation performed by Json_dom::find_child_doms()
  or Json_dom::seek() is done.

  @return true if only one result is needed and a result has been found
*/
template <class Result_vector>
static inline bool is_seek_done(const Result_vector *hits, bool only_need_one)
{
  return only_need_one && hits->size() > 0;
}


bool Json_dom::find_child_doms(const Json_path_leg *path_leg,
                               bool auto_wrap,
                               bool only_need_one,
                               Json_dom_vector *duplicates,
                               Json_dom_vector *result)
{
  enum_json_type dom_type= json_type();
  enum_json_path_leg_type leg_type= path_leg->get_type();

  if (is_seek_done(result, only_need_one))
    return false;

  // Handle auto-wrapping of non-arrays.
  if (auto_wrap && dom_type != enum_json_type::J_ARRAY &&
      path_leg->is_autowrap())
  {
    return !seen_already(result, this) &&
      add_if_missing(this, duplicates, result);
  }

  switch (leg_type)
  {
  case jpl_array_cell:
    if (dom_type == enum_json_type::J_ARRAY)
    {
      auto array= down_cast<const Json_array *>(this);
      Json_array_index idx= path_leg->first_array_index(array->size());
      return idx.within_bounds() &&
        add_if_missing((*array)[idx.position()], duplicates, result);
    }
    return false;
  case jpl_array_range:
  case jpl_array_cell_wildcard:
    if (dom_type == enum_json_type::J_ARRAY)
    {
      const auto array= down_cast<const Json_array *>(this);
      auto range= path_leg->get_array_range(array->size());
      for (size_t i= range.m_begin; i < range.m_end; ++i)
      {
        if (add_if_missing((*array)[i], duplicates, result))
          return true;                          /* purecov: inspected */
        if (is_seek_done(result, only_need_one))
          return false;
      }
    }
    return false;
  case jpl_ellipsis:
    {
      if (add_if_missing(this, duplicates, result))
        return true;                          /* purecov: inspected */

      if (dom_type == enum_json_type::J_ARRAY)
      {
        const Json_array * const array= down_cast<const Json_array *>(this);

        for (unsigned eidx= 0; eidx < array->size(); eidx++)
        {
          Json_dom * child= (*array)[eidx];
          if (add_if_missing(child, duplicates, result))
            return true;                      /* purecov: inspected */
          if (is_seek_done(result, only_need_one))
            return false;                     /* purecov: inspected */

          enum_json_type child_type= child->json_type();
          if ((child_type == enum_json_type::J_ARRAY) ||
              (child_type == enum_json_type::J_OBJECT))
          {
            // now recurse and add all objects and arrays under the child
            if (child->find_child_doms(path_leg, auto_wrap, only_need_one,
                                       duplicates, result))
              return true;                    /* purecov: inspected */
          }
        } // end of loop through children
      }
      else if (dom_type == enum_json_type::J_OBJECT)
      {
        const Json_object *const object=
          down_cast<const Json_object *>(this);

        for (Json_object::const_iterator iter= object->begin();
             iter != object->end(); ++iter)
        {
          Json_dom *child= iter->second;
          enum_json_type child_type= child->json_type();

          if (add_if_missing(child, duplicates, result))
            return true;                      /* purecov: inspected */
          if (is_seek_done(result, only_need_one))
            return false;                     /* purecov: inspected */

          if ((child_type == enum_json_type::J_ARRAY) ||
              (child_type == enum_json_type::J_OBJECT))
          {
            // now recurse and add all objects and arrays under the child
            if (child->find_child_doms(path_leg, auto_wrap, only_need_one,
                                       duplicates, result))
              return true;                    /* purecov: inspected */
          }
        } // end of loop through children
      }

      return false;
    }
  case jpl_member:
    {
      if (dom_type == enum_json_type::J_OBJECT)
      {
        const Json_object * object= down_cast<const Json_object *>(this);
        Json_dom *child= object->get(path_leg->get_member_name());

        if (child != NULL && add_if_missing(child, duplicates, result))
          return true;                        /* purecov: inspected */
      }

      return false;
    }
  case jpl_member_wildcard:
    {
      if (dom_type == enum_json_type::J_OBJECT)
      {
        const Json_object * object= down_cast<const Json_object *>(this);

        for (Json_object::const_iterator iter= object->begin();
             iter != object->end(); ++iter)
        {
          if (add_if_missing(iter->second, duplicates, result))
            return true;                      /* purecov: inspected */
          if (is_seek_done(result, only_need_one))
            return false;
        }
      }

      return false;
    }
  }

  /* purecov: begin deadcode */
  DBUG_ASSERT(false);
  return true;
  /* purecov: end */
}


Json_object::Json_object()
  : Json_dom(),
    m_map(Json_object_map::key_compare(),
          Json_object_map::allocator_type(key_memory_JSON))
{}


Json_object::~Json_object()
{
  clear();
}


/**
  Check if the depth of a JSON document exceeds the maximum supported
  depth (JSON_DOCUMENT_MAX_DEPTH). Raise an error if the maximum depth
  has been exceeded.

  @param[in] depth  the current depth of the document
  @return true if the maximum depth is exceeded, false otherwise
*/
static bool check_json_depth(size_t depth)
{
  if (depth > JSON_DOCUMENT_MAX_DEPTH)
  {
    my_error(ER_JSON_DOCUMENT_TOO_DEEP, MYF(0));
    return true;
  }
  return false;
}


namespace
{

/**
  This class implements rapidjson's Handler concept to make our own handler
  which will construct our DOM from the parsing of the JSON text.
  <code>
  bool Null() {   }
  bool Bool(bool) {   }
  bool Int(int) {   }
  bool Uint(unsigned) {   }
  bool Int64(int64_t) {   }
  bool Uint64(uint64_t) {   }
  bool Double(double) {   }
  bool RawNumber(const Ch*, SizeType, bool) {   }
  bool String(const Ch*, SizeType, bool) {   }
  bool StartObject() {   }
  bool Key() {   }
  bool EndObject(SizeType) {   }
  bool StartArray() {   }
  bool EndArray(SizeType) {   }
  </code>
  @see Json_dom::parse
*/
class Rapid_json_handler
{
private:

// std::cerr << "callback " << name << ':' << state << '\n'; std::cerr.flush()
#define DUMP_CALLBACK(name,state)

  enum enum_state
  {
    expect_anything,
    expect_array_value,
    expect_object_key,
    expect_object_value,
    expect_eof
  };

  enum_state m_state;   ///< Tells what kind of value to expect next.
  std::unique_ptr<Json_dom> m_dom_as_built; ///< Root of the DOM being built.
  Json_dom* m_current_element;  ///< The current object/array being parsed.
  size_t m_depth;      ///< The depth at which parsing currently happens.
  std::string m_key;   ///< The name of the current member of an object.
public:
  Rapid_json_handler()
    : m_state(expect_anything), m_dom_as_built(nullptr),
      m_current_element(nullptr), m_depth(0), m_key()
  {}

  /**
    @returns The built JSON DOM object.
    Deallocation of the returned value is the responsibility of the caller.
  */
  Json_dom *get_built_doc()
  {
    return m_dom_as_built.release();
  }

private:
  /**
    Function which is called on each value found in the JSON
    document being parsed.

    @param[in] value the value that was seen
    @return true if parsing should continue, false if an error was
            found and parsing should stop
  */
  bool seeing_value(Json_dom *value)
  {
    std::unique_ptr<Json_dom> uptr(value);
    if (value == nullptr || check_json_depth(m_depth + 1))
      return false;
    switch (m_state)
    {
    case expect_anything:
      m_dom_as_built= std::move(uptr);
      m_state= expect_eof;
      return true;
    case expect_array_value:
      {
        auto array= down_cast<Json_array *>(m_current_element);
        /*
          append_alias() only takes over ownership if successful, so don't
          release the unique_ptr unless the value was added to the array.
        */
        if (array->append_alias(value))
          return false;                         /* purecov: inspected */
        uptr.release();
        return true;
      }
    case expect_object_value:
      {
        m_state= expect_object_key;
        auto object= down_cast<Json_object *>(m_current_element);
        /*
          Hand over ownership from uptr to object. Contrary to append_alias(),
          add_alias() takes over ownership also in the case of failure.
        */
        return !object->add_alias(m_key, uptr.release());
      }
    default:
      /* purecov: begin inspected */
      DBUG_ASSERT(false);
      return false;
      /* purecov: end */
    }
  }

public:
  bool Null()
  {
    DUMP_CALLBACK("null", state);
    return seeing_value(new (std::nothrow) Json_null());
  }

  bool Bool(bool b)
  {
    DUMP_CALLBACK("bool", state);
    return seeing_value(new (std::nothrow) Json_boolean(b));
  }

  bool Int(int i)
  {
    DUMP_CALLBACK("int", state);
    return seeing_value(new (std::nothrow) Json_int(i));
  }

  bool Uint(unsigned u)
  {
    DUMP_CALLBACK("uint", state);
    return seeing_value(new (std::nothrow) Json_int(static_cast<longlong>(u)));
  }

  bool Int64(int64_t i)
  {
    DUMP_CALLBACK("int64", state);
    return seeing_value(new (std::nothrow) Json_int(i));
  }

  bool Uint64(uint64_t ui64)
  {
    DUMP_CALLBACK("uint64", state);
    return seeing_value(new (std::nothrow) Json_uint(ui64));
  }

  bool Double(double d)
  {
    DUMP_CALLBACK("double", state);
    /*
      We only accept finite values. RapidJSON normally stops non-finite values
      from getting here, but sometimes +/-inf values could end up here anyway.
    */
    if (!std::isfinite(d))
      return false;
    return seeing_value(new (std::nothrow) Json_double(d));
  }

  bool RawNumber(const char* str, SizeType length, bool)
  {
    char *end[]= { const_cast<char*>(str) + length };
    int error= 0;
    double value = my_strtod(str, end, &error);

    if (error == EOVERFLOW)
      return false;
    return Double(value);
  }

  bool String(const char* str, SizeType length, bool)
  {
    DUMP_CALLBACK("string", state);
    return seeing_value(new (std::nothrow) Json_string(str, length));
  }

  bool StartObject()
  {
    DUMP_CALLBACK("start object {", state);
    auto object= new (std::nothrow) Json_object();
    bool success= seeing_value(object);
    m_depth++;
    m_current_element= object;
    m_state= expect_object_key;
    return success;
  }

  bool EndObject(SizeType)
  {
    DUMP_CALLBACK("} end object", state);
    DBUG_ASSERT(m_state == expect_object_key);
    end_object_or_array();
    return true;
  }

  bool StartArray()
  {
    DUMP_CALLBACK("start array [", state);
    auto array= new (std::nothrow) Json_array();
    bool success= seeing_value(array);
    m_depth++;
    m_current_element= array;
    m_state= expect_array_value;
    return success;
  }

  bool EndArray(SizeType)
  {
    DUMP_CALLBACK("] end array", state);
    DBUG_ASSERT(m_state == expect_array_value);
    end_object_or_array();
    return true;
  }

  bool Key(const char* str, SizeType len, bool)
  {
    if (check_json_depth(m_depth + 1))
      return false;
    DBUG_ASSERT(m_state == expect_object_key);
    m_state= expect_object_value;
    m_key.assign(str, len);
    return true;
  }

private:
  void end_object_or_array()
  {
    m_depth--;
    m_current_element= m_current_element->parent();
    if (m_current_element == nullptr)
    {
      DBUG_ASSERT(m_depth == 0);
      m_state= expect_eof;
    }
    else if (m_current_element->json_type() == enum_json_type::J_OBJECT)
      m_state= expect_object_key;
    else
    {
      DBUG_ASSERT(m_current_element->json_type() == enum_json_type::J_ARRAY);
      m_state= expect_array_value;
    }
  }
};

} // namespace


Json_dom *Json_dom::parse(const char *text, size_t length,
                          const char **syntaxerr, size_t *offset,
                          bool handle_numbers_as_double)
{
  Rapid_json_handler handler;
  MemoryStream ss(text, length);
  Reader reader;
  bool success=
    handle_numbers_as_double ?
    reader.Parse<kParseNumbersAsStringsFlag>(ss, handler) :
    reader.Parse<kParseDefaultFlags>(ss, handler);

  if (success)
  {
    Json_dom *dom= handler.get_built_doc();
    if (dom == NULL && syntaxerr != NULL)
    {
      // The parsing failed for some other reason than a syntax error.
      *syntaxerr= NULL;
    }
    return dom;
  }

  // Report the error offset and the error message if requested by the caller.
  if (offset != NULL)
    *offset= reader.GetErrorOffset();
  if (syntaxerr != NULL)
    *syntaxerr= GetParseError_En(reader.GetParseErrorCode());

  return NULL;
}


namespace
{

/**
  This class implements a handler for use with rapidjson::Reader when
  we want to check if a string is a valid JSON text. The handler does
  not build a DOM structure, so it is quicker than Json_dom::parse()
  in the cases where we don't care about the DOM, such as in the
  JSON_VALID() function.

  The handler keeps track of how deeply nested the document is, and it
  raises an error and stops parsing when the depth exceeds
  JSON_DOCUMENT_MAX_DEPTH.
*/
class Syntax_check_handler
{
private:
  size_t m_depth;        ///< The current depth of the document

  bool seeing_scalar()
  {
    return !check_json_depth(m_depth + 1);
  }

public:
  Syntax_check_handler() : m_depth(0) {}

  /*
    These functions are callbacks used by rapidjson::Reader when
    parsing a JSON document. They all follow the rapidjson convention
    of returning true on success and false on failure.
  */
  bool StartObject() { return !check_json_depth(++m_depth); }
  bool EndObject(SizeType) { --m_depth; return true; }
  bool StartArray() { return !check_json_depth(++m_depth); }
  bool EndArray(SizeType) { --m_depth; return true; }
  bool Null() { return seeing_scalar(); }
  bool Bool(bool) { return seeing_scalar(); }
  bool Int(int) { return seeing_scalar(); }
  bool Uint(unsigned) { return seeing_scalar(); }
  bool Int64(int64_t) { return seeing_scalar(); }
  bool Uint64(uint64_t) { return seeing_scalar(); }
  bool Double(double, bool is_int MY_ATTRIBUTE((unused)) = false)
  { return seeing_scalar(); }
  bool String(const char*, SizeType, bool) { return seeing_scalar(); }
  bool Key(const char*, SizeType, bool) { return seeing_scalar(); }
  bool RawNumber(const char*, SizeType, bool)
  { return seeing_scalar(); }
};

} // namespace


bool is_valid_json_syntax(const char *text, size_t length)
{
  Syntax_check_handler handler;
  Reader reader;
  MemoryStream ms(text, length);
  return reader.Parse<rapidjson::kParseDefaultFlags>(ms, handler);
}


/**
  Map the JSON type used by the binary representation to the type
  used by Json_dom and Json_wrapper.

  Note: Does not look into opaque values to determine if they
  represent decimal or date/time values. For that, look into the
  Value an retrive field_type.

  @param[in]  bintype
  @returns the JSON_dom JSON type.
*/
static enum_json_type bjson2json(const json_binary::Value::enum_type bintype)
{
  enum_json_type res= enum_json_type::J_ERROR;

  switch (bintype)
  {
  case json_binary::Value::STRING:
    res= enum_json_type::J_STRING;
    break;
  case json_binary::Value::INT:
    res= enum_json_type::J_INT;
    break;
  case json_binary::Value::UINT:
    res= enum_json_type::J_UINT;
    break;
  case json_binary::Value::DOUBLE:
    res= enum_json_type::J_DOUBLE;
    break;
  case json_binary::Value::LITERAL_TRUE:
  case json_binary::Value::LITERAL_FALSE:
    res= enum_json_type::J_BOOLEAN;
    break;
  case json_binary::Value::LITERAL_NULL:
    res= enum_json_type::J_NULL;
    break;
  case json_binary::Value::ARRAY:
    res= enum_json_type::J_ARRAY;
    break;
  case json_binary::Value::OBJECT:
    res= enum_json_type::J_OBJECT;
    break;
  case json_binary::Value::ERROR:
    res= enum_json_type::J_ERROR;
    break;
  case json_binary::Value::OPAQUE:
    res= enum_json_type::J_OPAQUE;
    break;
  }

  return res;
}


Json_dom *Json_dom::parse(const THD* thd, const json_binary::Value &v)
{
  std::unique_ptr<Json_dom> dom(json_binary_to_dom_template(v));
  if (dom.get() == NULL || populate_object_or_array(thd, dom.get(), v))
    return NULL;                              /* purecov: inspected */
  return dom.release();
}


/// Get string data as std::string from a json_binary::Value.
static std::string get_string_data(const json_binary::Value &v)
{
  return std::string(v.get_data(), v.get_data_length());
}


/**
  Create a DOM template for the provided json_binary::Value.

  If the binary value represents a scalar, create a Json_dom object
  that represents the scalar and return a pointer to it.

  If the binary value represents an object or an array, create an
  empty Json_object or Json_array object and return a pointer to it.

  @param v  the binary value to convert to DOM

  @return a DOM template for the top-level the binary value, or NULL
  if an error is detected.
*/
static Json_dom *json_binary_to_dom_template(const json_binary::Value &v)
{
  switch (v.type())
  {
  case json_binary::Value::OBJECT:
    return new (std::nothrow) Json_object();
  case json_binary::Value::ARRAY:
    return new (std::nothrow) Json_array();
  case json_binary::Value::DOUBLE:
    return new (std::nothrow) Json_double(v.get_double());
  case json_binary::Value::INT:
    return new (std::nothrow) Json_int(v.get_int64());
  case json_binary::Value::UINT:
    return new (std::nothrow) Json_uint(v.get_uint64());
  case json_binary::Value::LITERAL_FALSE:
    return new (std::nothrow) Json_boolean(false);
  case json_binary::Value::LITERAL_TRUE:
    return new (std::nothrow) Json_boolean(true);
  case json_binary::Value::LITERAL_NULL:
    return new (std::nothrow) Json_null();
  case json_binary::Value::OPAQUE:
    {
      const enum_field_types ftyp= v.field_type();

      if (ftyp == MYSQL_TYPE_NEWDECIMAL)
      {
        my_decimal m;
        if (Json_decimal::convert_from_binary(v.get_data(),
                                              v.get_data_length(),
                                              &m))
          return NULL;                        /* purecov: inspected */
        return new (std::nothrow) Json_decimal(m);
      }

      if (ftyp == MYSQL_TYPE_DATE ||
          ftyp == MYSQL_TYPE_TIME ||
          ftyp == MYSQL_TYPE_DATETIME ||
          ftyp == MYSQL_TYPE_TIMESTAMP)
      {
        MYSQL_TIME t;
        Json_datetime::from_packed(v.get_data(), ftyp, &t);
        return new (std::nothrow) Json_datetime(t, ftyp);
      }

      return new (std::nothrow) Json_opaque(v.field_type(),
                                            v.get_data(),
                                            v.get_data_length());
    }
  case json_binary::Value::STRING:
    return new (std::nothrow) Json_string(v.get_data(), v.get_data_length());
  case json_binary::Value::ERROR:
    break;                                    /* purecov: inspected */
  }

  /* purecov: begin inspected */
  my_error(ER_INVALID_JSON_BINARY_DATA, MYF(0));
  return NULL;
  /* purecov: end */
}


/**
  Populate the DOM representation of a JSON object or array with the
  elements found in a binary JSON object or array. If the supplied
  value does not represent an object or an array, do nothing.

  @param[in]     thd    THD handle
  @param[in,out] dom    the Json_dom object to populate
  @param[in]     v      the binary JSON value to read from

  @retval true on error
  @retval false on success
*/
static bool populate_object_or_array(const THD *thd, Json_dom *dom,
                                     const json_binary::Value &v)
{
  switch (v.type())
  {
  case json_binary::Value::OBJECT:
    // Check that we haven't run out of stack before we dive into the object.
    return check_stack_overrun(thd, STACK_MIN_SIZE, nullptr) ||
      populate_object(thd, down_cast<Json_object *>(dom), v);
  case json_binary::Value::ARRAY:
    // Check that we haven't run out of stack before we dive into the array.
    return check_stack_overrun(thd, STACK_MIN_SIZE, nullptr) ||
      populate_array(thd, down_cast<Json_array *>(dom), v);
  default:
    return false;
  }
}


/**
  Populate the DOM representation of a JSON object with the key/value
  pairs found in a binary JSON object.

  @param[in]     thd    THD handle
  @param[in,out] jo     the JSON object to populate
  @param[in]     v      the binary JSON object to read from

  @retval true on error
  @retval false on success
*/
static bool populate_object(const THD *thd, Json_object *jo,
                            const json_binary::Value &v)
{
  for (uint32 i= 0; i < v.element_count(); i++)
  {
    auto key= get_string_data(v.key(i));
    auto val= v.element(i);
    auto dom= json_binary_to_dom_template(val);
    if (jo->add_alias(key, dom) || populate_object_or_array(thd, dom, val))
    {
      // No need to delete dom on error, as Json_object does that for us.
      return true;                            /* purecov: inspected */
    }
  }
  return false;
}


/**
  Populate the DOM representation of a JSON array with the elements
  found in a binary JSON array.

  @param[in]     thd    THD handle
  @param[in,out] ja     the JSON array to populate
  @param[in]     v      the binary JSON array to read from

  @retval true on error
  @retval false on success
*/
static bool populate_array(const THD *thd, Json_array *ja,
                           const json_binary::Value &v)
{
  for (uint32 i= 0; i < v.element_count(); i++)
  {
    auto elt= v.element(i);
    auto dom= json_binary_to_dom_template(elt);
    if (ja->append_alias(dom))
    {
      // Need to delete dom on error. append_alias() doesn't do it for us.
      /* purecov: begin inspected */
      delete dom;
      return true;
      /* purecov: end */
    }
    if (populate_object_or_array(thd, dom, elt))
      return true;                            /* purecov: inspected */
  }
  return false;
}


void Json_array::replace_dom_in_container(Json_dom *oldv, Json_dom *newv)
{
  auto it= std::find(m_v.begin(), m_v.end(), oldv);
  if (it != m_v.end())
  {
    delete oldv;
    *it= newv;
    newv->set_parent(this);
  }
}


void Json_object::replace_dom_in_container(Json_dom *oldv, Json_dom *newv)
{
  for (Json_object_map::iterator it= m_map.begin(); it != m_map.end(); ++it)
  {
    if (it->second == oldv)
    {
      delete oldv;
      it->second= newv;
      newv->set_parent(this);
      break;
    }
  }
}


bool Json_object::add_clone(const std::string &key, const Json_dom *value)
{
  if (!value)
    return true;                                /* purecov: inspected */
  return add_alias(key, value->clone());
}


bool Json_object::add_alias(const std::string &key, Json_dom *value)
{
  if (!value)
    return true;                                /* purecov: inspected */

  /*
    Wrap value in a unique_ptr to make sure it's released if we cannot
    add it to the object. The contract of add_alias() requires that it
    either gets added to the object or gets deleted.
  */
  std::unique_ptr<Json_dom> uptr(value);

  /*
    We have already an element with this key.  Note we compare utf-8 bytes
    directly here. It's complicated when when you take into account composed
    and decomposed forms of accented characters and ligatures: different
    sequences might encode the same glyphs but we ignore that for now.  For
    example, the code point U+006E (the Latin lowercase "n") followed by
    U+0303 (the combining tilde) is defined by Unicode to be canonically
    equivalent to the single code point U+00F1 (the lowercase letter of the
    Spanish alphabet).  For now, users must normalize themselves to avoid
    element dups.

    This is what ECMAscript does also: "Two IdentifierName that are
    canonically equivalent according to the Unicode standard are not equal
    unless they are represented by the exact same sequence of code units (in
    other words, conforming ECMAScript implementations are only required to
    do bitwise comparison on IdentifierName values). The intent is that the
    incoming source text has been converted to normalised form C before it
    reaches the compiler." (ECMA-262 5.1 edition June 2011)

    See WL-2048 Add function for Unicode normalization
  */
  std::pair<Json_object_map::const_iterator, bool> ret=
    m_map.insert(std::make_pair(key, value));

  if (ret.second)
  {
    // the element was inserted
    value->set_parent(this);
    uptr.release();
  }

  return false;
}

bool Json_object::consume(Json_object *other)
{
  // We've promised to delete other before returning.
  std::unique_ptr<Json_object> uptr(other);

  Json_object_map &this_map= m_map;
  Json_object_map &other_map= other->m_map;

  for (Json_object_map::iterator other_iter= other_map.begin();
       other_iter != other_map.end(); other_map.erase(other_iter++))
  {
    const std::string &key= other_iter->first;
    Json_dom *value= other_iter->second;
    other_iter->second= NULL;

    Json_object_map::iterator this_iter= this_map.find(key);

    if (this_iter == this_map.end())
    {
      // The key does not exist in this object, so add the key/value pair.
      if (add_alias(key, value))
        return true;                          /* purecov: inspected */
    }
    else
    {
      /*
        Oops. Duplicate key. Merge the values.
        This is where the recursion in JSON_MERGE() occurs.
      */
      this_iter->second= merge_doms(this_iter->second, value);
      if (this_iter->second == NULL)
        return true;                          /* purecov: inspected */
      this_iter->second->set_parent(this);
    }
  }

  return false;
}

Json_dom *Json_object::get(const std::string &key) const
{
  const Json_object_map::const_iterator iter= m_map.find(key);

  if (iter != m_map.end())
  {
    DBUG_ASSERT(iter->second->parent() == this);
    return iter->second;
  }

  return NULL;
}


bool Json_object::remove(const std::string &key)
{
  auto it= m_map.find(key);
  if (it == m_map.end())
    return false;

  delete it->second;
  m_map.erase(it);
  return true;
}


size_t Json_object::cardinality() const
{
  return m_map.size();
}


uint32 Json_object::depth() const
{
  uint deepest_child= 0;

  for (Json_object_map::const_iterator iter= m_map.begin();
       iter != m_map.end(); ++iter)
  {
    deepest_child= std::max(deepest_child, iter->second->depth());
  }
  return 1 + deepest_child;
}


Json_dom *Json_object::clone() const
{
  Json_object * const o= new (std::nothrow) Json_object();
  if (!o)
    return NULL;                                /* purecov: inspected */

  for (Json_object_map::const_iterator iter= m_map.begin();
       iter != m_map.end(); ++iter)
  {
    if (o->add_clone(iter->first, iter->second))
    {
      delete o;                                 /* purecov: inspected */
      return NULL;                              /* purecov: inspected */
    }
  }
  return o;
}


void Json_object::clear()
{
  for (Json_object_map::const_iterator iter= m_map.begin();
       iter != m_map.end(); ++iter)
  {
    delete iter->second;
  }
  m_map.clear();
}


/**
  Compare two keys from a JSON object and determine whether or not the
  first key is less than the second key. key1 is considered less than
  key2 if

  a) key1 is shorter than key2, or if

  b) key1 and key2 have the same length, but different contents, and
  the first byte that differs has a smaller value in key1 than in key2

  Otherwise, key1 is not less than key2.

  @param key1 the first key to compare
  @param key2 the second key to compare
  @return true if key1 is considered less than key2, false otherwise
*/
bool Json_key_comparator::operator() (const std::string &key1,
                                      const std::string &key2) const
{
  if (key1.length() != key2.length())
    return key1.length() < key2.length();

  return memcmp(key1.data(), key2.data(), key1.length()) < 0;
}


Json_array::Json_array()
  : Json_dom(), m_v(Malloc_allocator<Json_dom*>(key_memory_JSON))
{}


Json_array::Json_array(Json_dom *innards)
  : Json_array()
{
  append_alias(innards);
}


Json_array::~Json_array()
{
  delete_container_pointers(m_v);
}


bool Json_array::append_clone(const Json_dom *value)
{
  return insert_clone(size(), value);
}


bool Json_array::append_alias(Json_dom *value)
{
  return insert_alias(size(), value);
}


bool Json_array::consume(Json_array *other)
{
  // We've promised to delete other before returning.
  std::unique_ptr<Json_array> aptr(other);

  m_v.reserve(size() + other->size());
  for (auto iter= other->m_v.begin(); iter != other->m_v.end(); ++iter)
  {
    if (append_alias(*iter))
      return true;                              /* purecov: inspected */
    *iter= nullptr;
  }

  return false;
}


bool Json_array::insert_clone(size_t index, const Json_dom *value)
{
  if (!value)
    return true;                                /* purecov: inspected */
  return insert_alias(index, value->clone());
}


bool Json_array::insert_alias(size_t index, Json_dom *value)
{
  if (!value)
    return true;                                /* purecov: inspected */

  /*
    Insert the value at the given index, or at the end of the array if the
    index points past the end of the array.
  */
  auto pos= m_v.begin() + std::min(m_v.size(), index);
  m_v.insert(pos, value);
  value->set_parent(this);
  return false;
}


bool Json_array::remove(size_t index)
{
  if (index < m_v.size())
  {
    auto iter= m_v.begin() + index;
    delete *iter;
    m_v.erase(iter);
    return true;
  }

  return false;
}


uint32 Json_array::depth() const
{
  uint deepest_child= 0;

  for (auto child : m_v)
  {
    deepest_child= std::max(deepest_child, child->depth());
  }
  return 1 + deepest_child;
}

Json_dom *Json_array::clone() const
{
  std::unique_ptr<Json_array> vv(new (std::nothrow) Json_array());
  if (vv == nullptr)
    return nullptr;                             /* purecov: inspected */

  vv->m_v.reserve(size());
  for (auto child : m_v)
  {
    if (vv->append_clone(child))
      return nullptr;                           /* purecov: inspected */
  }

  return vv.release();
}


void Json_array::clear()
{
  delete_container_pointers(m_v);
}


/**
  Perform quoting on a JSON string to make an external representation
  of it. it wraps double quotes (text quotes) around the string (cptr)
  an also performs escaping according to the following table:
  <pre>
  @verbatim
  Common name     C-style  Original unescaped     Transformed to
                  escape   UTF-8 bytes            escape sequence
                  notation                        in UTF-8 bytes
  ---------------------------------------------------------------
  quote           \"       %x22                    %x5C %x22
  backslash       \\       %x5C                    %x5C %x5C
  backspace       \b       %x08                    %x5C %x62
  formfeed        \f       %x0C                    %x5C %x66
  linefeed        \n       %x0A                    %x5C %x6E
  carriage-return \r       %x0D                    %x5C %x72
  tab             \t       %x09                    %x5C %x74
  unicode         \uXXXX  A hex number in the      %x5C %x75
                          range of 00-1F,          followed by
                          except for the ones      4 hex digits
                          handled above (backspace,
                          formfeed, linefeed,
                          carriage-return,
                          and tab).
  ---------------------------------------------------------------
  @endverbatim
  </pre>

  @param[in] cptr pointer to string data
  @param[in] length the length of the string
  @param[in,out] buf the destination buffer
  @retval false on success
  @retval true on error
*/
bool double_quote(const char *cptr, size_t length, String *buf)
{
  if (buf->append('"'))
    return true;                              /* purecov: inspected */

  for (size_t i= 0; i < length; i++)
  {
    char esc[2]= {'\\', cptr[i]};
    bool done= true;
    switch (cptr[i])
    {
    case '"' :
    case '\\' :
      break;
    case '\b':
      esc[1]= 'b';
      break;
    case '\f':
      esc[1]= 'f';
      break;
    case '\n':
      esc[1]= 'n';
      break;
    case '\r':
      esc[1]= 'r';
      break;
    case '\t':
      esc[1]= 't';
      break;
    default:
      done= false;
    }

    if (done)
    {
      if (buf->append(esc[0]) || buf->append(esc[1]))
        return true;                          /* purecov: inspected */
    }
    else if (((cptr[i] & ~0x7f) == 0) && // bit 8 not set
             (cptr[i] < 0x1f))
    {
      /*
        Unprintable control character, use hex a hexadecimal number.
        The meaning of such a number determined by ISO/IEC 10646.
      */
      if (buf->append("\\u00") ||
          buf->append(_dig_vec_lower[(cptr[i] & 0xf0) >> 4]) ||
          buf->append(_dig_vec_lower[(cptr[i] & 0x0f)]))
        return true;                          /* purecov: inspected */
    }
    else if (buf->append(cptr[i]))
    {
      return true;                            /* purecov: inspected */
    }
  }
  return buf->append('"');
}


Json_decimal::Json_decimal(const my_decimal &value)
  : Json_number(), m_dec(value)
{}


int Json_decimal::binary_size() const
{
  /*
    We need two bytes for the precision and the scale, plus whatever
    my_decimal2binary() needs.
  */
  return 2 + my_decimal_get_binary_size(m_dec.precision(), m_dec.frac);
}


bool Json_decimal::get_binary(char* dest) const
{
  DBUG_ASSERT(binary_size() <= MAX_BINARY_SIZE);
  /*
    my_decimal2binary() loses the precision and the scale, so store them
    in the first two bytes.
  */
  dest[0]= static_cast<char>(m_dec.precision());
  dest[1]= static_cast<char>(m_dec.frac);
  // Then store the decimal value.
  return my_decimal2binary(E_DEC_ERROR, &m_dec,
                           pointer_cast<uchar*>(dest) + 2,
                           m_dec.precision(), m_dec.frac) != E_DEC_OK;
}


bool Json_decimal::convert_from_binary(const char *bin, size_t len,
                                       my_decimal *dec)
{
  // Expect at least two bytes, which contain precision and scale.
  bool error= (len < 2);

  if (!error)
  {
    int precision= bin[0];
    int scale= bin[1];

    // The decimal value is encoded after the two precision/scale bytes.
    size_t bin_size= my_decimal_get_binary_size(precision, scale);
    error=
      (bin_size != len - 2) ||
      (binary2my_decimal(E_DEC_ERROR,
                         pointer_cast<const uchar*>(bin) + 2,
                         dec, precision, scale) != E_DEC_OK);
  }

  if (error)
    my_error(ER_INVALID_JSON_BINARY_DATA, MYF(0)); /* purecov: inspected */

  return error;
}


Json_dom *Json_double::clone() const
{
  return new (std::nothrow) Json_double(m_f);
}


enum_json_type Json_datetime::json_type() const
{
  switch (m_field_type)
  {
  case MYSQL_TYPE_TIME:
    return enum_json_type::J_TIME;
  case MYSQL_TYPE_DATETIME:
    return enum_json_type::J_DATETIME;
  case MYSQL_TYPE_DATE:
    return enum_json_type::J_DATE;
  case MYSQL_TYPE_TIMESTAMP:
    return enum_json_type::J_TIMESTAMP;
  default: ;
  }
  /* purecov: begin inspected */
  DBUG_ASSERT(false);
  return enum_json_type::J_NULL;
  /* purecov: end inspected */
}


Json_dom *Json_datetime::clone() const
{
  return new (std::nothrow) Json_datetime(m_t, m_field_type);
}


void Json_datetime::to_packed(char *dest) const
{
  longlong packed= TIME_to_longlong_packed(&m_t);
  int8store(dest, packed);
}


void Json_datetime::from_packed(const char *from, enum_field_types ft,
                                MYSQL_TIME *to)
{
  TIME_from_longlong_packed(to, ft, sint8korr(from));
}


Json_dom *Json_opaque::clone() const
{
  return new (std::nothrow) Json_opaque(m_mytype, value(), size());
}


Json_wrapper_object_iterator::
Json_wrapper_object_iterator(const Json_object *obj)
  : m_is_dom(true), m_iter(obj->begin()), m_end(obj->end()), m_element_count(-1)
{}


Json_wrapper_object_iterator::
Json_wrapper_object_iterator(const json_binary::Value *value)
  : m_is_dom(false), m_element_count(value->element_count()), m_value(value)
{
  m_curr_element= 0;
}


bool Json_wrapper_object_iterator::empty() const
{
  return m_is_dom ? (m_iter == m_end) : (m_curr_element >= m_element_count);
}


void Json_wrapper_object_iterator::next()
{
  if (m_is_dom)
  {
    m_iter++;
  }
  else
  {
    ++m_curr_element;
  }
}


std::pair<const std::string, Json_wrapper>
Json_wrapper_object_iterator::elt() const
{
  if (m_is_dom)
  {
    Json_wrapper wr(m_iter->second);
    // DOM possibly owned by object and we don't want to make a clone
    wr.set_alias();
    return std::make_pair(m_iter->first, wr);
  }

  return std::make_pair(get_string_data(m_value->key(m_curr_element)),
                        Json_wrapper(m_value->element(m_curr_element)));
}


Json_wrapper::Json_wrapper(Json_dom *dom_value)
  : m_dom_value(dom_value), m_is_dom(true)
{
  // Workaround for Solaris Studio, initialize in CTOR body
  m_dom_alias= false;
  if (!dom_value)
  {
    m_dom_alias= true; //!< no deallocation, make us empty
  }
}


Json_wrapper::Json_wrapper(Json_wrapper &&old)
  : m_is_dom(old.m_is_dom)
{
  if (m_is_dom)
  {
    m_dom_alias= old.m_dom_alias;
    m_dom_value= old.m_dom_value;
    // Mark old as aliased. Any ownership is effectively transferred to this.
    old.set_alias();
  }
  else
  {
    m_value= std::move(old.m_value);
  }
}

Json_wrapper::Json_wrapper(const json_binary::Value &value)
  : m_value(value), m_is_dom(false)
{}


Json_wrapper::Json_wrapper(const Json_wrapper &old)
  : m_is_dom(old.m_is_dom)
{
  if (m_is_dom)
  {
    m_dom_alias= old.m_dom_alias;
    m_dom_value= m_dom_alias ? old.m_dom_value : old.m_dom_value->clone();
  }
  else
  {
    m_value= old.m_value;
  }
}


Json_wrapper::~Json_wrapper()
{
  if (m_is_dom && !m_dom_alias)
  {
    // we own our own copy, so we are responsible for deallocation
    delete m_dom_value;
  }
}


/**
  Common implementation of move-assignment and copy-assignment for
  Json_wrapper. If @a from is an rvalue, its contents are moved into
  @a to, otherwise the contents are copied over.
*/
template <typename T>
static Json_wrapper &assign_json_wrapper(T &&from, Json_wrapper *to)
{
  if (&from == to)
  {
    return *to;   // self assignment: no-op
  }

  // Deallocate DOM if needed.
  to->~Json_wrapper();

  // Move or copy the value into the destination.
  new (to) Json_wrapper(std::forward<T>(from));

  return *to;
}


Json_wrapper &Json_wrapper::operator=(const Json_wrapper &from)
{
  return assign_json_wrapper(from, this);
}


Json_wrapper &Json_wrapper::operator=(Json_wrapper &&from)
{
  return assign_json_wrapper(std::move(from), this);
}


Json_dom *Json_wrapper::to_dom(const THD *thd)
{
  if (!m_is_dom)
  {
    // Build a DOM from the binary JSON value and
    // convert this wrapper to hold the DOM instead
    m_dom_value= Json_dom::parse(thd, m_value);
    m_is_dom= true;
    m_dom_alias= false;
  }

  return m_dom_value;
}


Json_dom *Json_wrapper::clone_dom(const THD *thd) const
{
  // If we already have a DOM, return a clone of it.
  if (m_is_dom)
    return m_dom_value ? m_dom_value->clone() : NULL;

  // Otherwise, produce a new DOM tree from the binary representation.
  return Json_dom::parse(thd, m_value);
}


bool Json_wrapper::to_binary(const THD *thd, String *str) const
{
  if (empty())
  {
    /* purecov: begin inspected */
    my_error(ER_INVALID_JSON_BINARY_DATA, MYF(0));
    return true;
    /* purecov: end */
  }

  if (m_is_dom)
    return json_binary::serialize(thd, m_dom_value, str);

  return m_value.raw_binary(thd, str);
}


/**
  Possibly append a single quote to a buffer.
  @param[in,out] buffer receiving buffer
  @param[in] json_quoted whether or not a quote should be appended
  @return false if successful, true on error
*/
static inline bool single_quote(String *buffer, bool json_quoted)
{
  return json_quoted && buffer->append('"');
}

/**
   Pretty-print a string to an evolving buffer, double-quoting if
   requested.

   @param[in] buffer the buffer to print to
   @param[in] json_quoted true if we should double-quote
   @param[in] data the string to print
   @param[in] length the string's length
   @return false on success, true on failure
*/
static int print_string(String *buffer, bool json_quoted,
                        const char *data, size_t length)
{
  return json_quoted ?
    double_quote(data, length, buffer) :
    buffer->append(data, length);
}


/**
  Helper function for wrapper_to_string() which adds a newline and indentation
  up to the specified level.

  @param[in,out] buffer  the buffer to write to
  @param[in]     level   how many nesting levels to add indentation for
  @retval false on success
  @retval true on error
*/
static bool newline_and_indent(String *buffer, size_t level)
{
  // Append newline and two spaces per indentation level.
  return buffer->append('\n') ||
    buffer->fill(buffer->length() + level * 2, ' ');
}


/**
  Helper function which does all the heavy lifting for
  Json_wrapper::to_string(). It processes the Json_wrapper
  recursively. The depth parameter keeps track of the current nesting
  level. When it reaches JSON_DOCUMENT_MAX_DEPTH, it gives up in order
  to avoid running out of stack space.

  @param[in]     wr          the value to convert to a string
  @param[in,out] buffer      the buffer to write to
  @param[in]     json_quoted quote strings if true
  @param[in]     pretty      add newlines and indentation if true
  @param[in]     func_name   the name of the calling function
  @param[in]     depth       the nesting level of @a wr

  @retval false on success
  @retval true on error
*/
static bool wrapper_to_string(const Json_wrapper &wr, String *buffer,
                              bool json_quoted, bool pretty,
                              const char *func_name, size_t depth)
{
  if (check_json_depth(++depth))
    return true;

  switch (wr.type())
  {
  case enum_json_type::J_TIME:
  case enum_json_type::J_DATE:
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_TIMESTAMP:
    {
      // Make sure the buffer has space for the datetime and the quotes.
      if (buffer->reserve(MAX_DATE_STRING_REP_LENGTH + 2))
        return true;                           /* purecov: inspected */
      MYSQL_TIME t;
      wr.get_datetime(&t);
      if (single_quote(buffer, json_quoted))
        return true;                           /* purecov: inspected */
      char *ptr= const_cast<char *>(buffer->ptr()) + buffer->length();
      const int size= my_TIME_to_str(&t, ptr, 6);
      buffer->length(buffer->length() + size);
      if (single_quote(buffer, json_quoted))
        return true;                           /* purecov: inspected */
      break;
    }
  case enum_json_type::J_ARRAY:
    {
      if (buffer->append('['))
        return true;                           /* purecov: inspected */

      size_t array_len= wr.length();
      for (uint32 i= 0; i < array_len; ++i)
      {
        if (i > 0 && buffer->append(pretty ? "," : ", "))
          return true;                         /* purecov: inspected */

        if (pretty && newline_and_indent(buffer, depth))
          return true;                         /* purecov: inspected */

        if (wrapper_to_string(wr[i], buffer, true, pretty, func_name, depth))
          return true;                         /* purecov: inspected */
      }

      if (pretty && array_len > 0 && newline_and_indent(buffer, depth - 1))
        return true;                           /* purecov: inspected */

      if (buffer->append(']'))
        return true;                           /* purecov: inspected */

      break;
    }
  case enum_json_type::J_BOOLEAN:
    if (buffer->append(wr.get_boolean() ? "true" : "false"))
      return true;                             /* purecov: inspected */
    break;
  case enum_json_type::J_DECIMAL:
    {
      int length= DECIMAL_MAX_STR_LENGTH + 1;
      if (buffer->reserve(length))
        return true;                           /* purecov: inspected */
      char *ptr= const_cast<char *>(buffer->ptr()) + buffer->length();
      my_decimal m;
      if (wr.get_decimal_data(&m) ||
          decimal2string(&m, ptr, &length, 0, 0, 0))
        return true;                           /* purecov: inspected */
      buffer->length(buffer->length() + length);
      break;
    }
  case enum_json_type::J_DOUBLE:
    {
      if (buffer->reserve(MY_GCVT_MAX_FIELD_WIDTH + 1))
        return true;                           /* purecov: inspected */
      double d= wr.get_double();
      size_t len= my_gcvt(d, MY_GCVT_ARG_DOUBLE, MY_GCVT_MAX_FIELD_WIDTH,
                          const_cast<char *>(buffer->ptr()) + buffer->length(),
                          NULL);
      buffer->length(buffer->length() + len);
      break;
    }
  case enum_json_type::J_INT:
    {
      if (buffer->append_longlong(wr.get_int()))
        return true;                           /* purecov: inspected */
      break;
    }
  case enum_json_type::J_NULL:
    if (buffer->append("null"))
      return true;                             /* purecov: inspected */
    break;
  case enum_json_type::J_OBJECT:
    {
      if (buffer->append('{'))
        return true;                           /* purecov: inspected */

      bool first= true;
      for (Json_wrapper_object_iterator iter= wr.object_iterator();
           !iter.empty(); iter.next())
      {
        if (!first && buffer->append(pretty ? "," : ", "))
          return true;                         /* purecov: inspected */

        first= false;

        if (pretty && newline_and_indent(buffer, depth))
          return true;                         /* purecov: inspected */

        const std::string &key= iter.elt().first;
        const char *key_data= key.c_str();
        size_t key_length= key.length();
        if (print_string(buffer, true, key_data, key_length) ||
            buffer->append(": ") ||
            wrapper_to_string(iter.elt().second, buffer, true, pretty,
                              func_name, depth))
          return true;                         /* purecov: inspected */
      }

      if (pretty && wr.length() > 0 && newline_and_indent(buffer, depth - 1))
        return true;                           /* purecov: inspected */

      if (buffer->append('}'))
        return true;                           /* purecov: inspected */

      break;
    }
  case enum_json_type::J_OPAQUE:
    {
      if (wr.get_data_length() > base64_encode_max_arg_length())
      {
        /* purecov: begin inspected */
        buffer->append("\"<data too long to decode - unexpected error>\"");
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "JSON: could not decode opaque data");
        return true;
        /* purecov: end */
      }

      const size_t needed=
        static_cast<size_t>(base64_needed_encoded_length(wr.get_data_length()));

      if (single_quote(buffer, json_quoted) ||
          buffer->append("base64:type") ||
          buffer->append_ulonglong(wr.field_type()) ||
          buffer->append(':'))
        return true;                           /* purecov: inspected */

      // "base64:typeXX:<binary data>"
      size_t pos= buffer->length();
      if (buffer->reserve(needed) ||
          base64_encode(wr.get_data(), wr.get_data_length(),
                        const_cast<char*>(buffer->ptr() + pos)))
        return true;                           /* purecov: inspected */
      buffer->length(pos + needed - 1); // drop zero terminator space
      if (single_quote(buffer, json_quoted))
        return true;                           /* purecov: inspected */
      break;
    }
  case enum_json_type::J_STRING:
    {
      const char *data= wr.get_data();
      size_t length= wr.get_data_length();

      if (print_string(buffer, json_quoted, data, length))
        return true;                           /* purecov: inspected */
      break;
    }
  case enum_json_type::J_UINT:
    {
      if (buffer->append_ulonglong(wr.get_uint()))
        return true;                           /* purecov: inspected */
      break;
    }
  default:
    /* purecov: begin inspected */
    DBUG_ASSERT(false);
    my_error(ER_INTERNAL_ERROR, MYF(0), "JSON wrapper: unexpected type");
    return true;
    /* purecov: end inspected */
  }

  if (buffer->length() > current_thd->variables.max_allowed_packet)
  {
    push_warning_printf(current_thd, Sql_condition::SL_WARNING,
			ER_WARN_ALLOWED_PACKET_OVERFLOWED,
			ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
			func_name, current_thd->variables.max_allowed_packet);
    return true;
  }

  return false;
}


bool Json_wrapper::to_string(String *buffer, bool json_quoted,
                             const char *func_name) const
{
  buffer->set_charset(&my_charset_utf8mb4_bin);
  return wrapper_to_string(*this, buffer, json_quoted, false, func_name, 0);
}


bool Json_wrapper::to_pretty_string(String *buffer, const char *func_name) const
{
  buffer->set_charset(&my_charset_utf8mb4_bin);
  return wrapper_to_string(*this, buffer, true, true, func_name, 0);
}


enum_json_type Json_wrapper::type() const
{
  if (empty())
  {
    return enum_json_type::J_ERROR;
  }

  if (m_is_dom)
  {
    return m_dom_value->json_type();
  }

  json_binary::Value::enum_type typ= m_value.type();

  if (typ == json_binary::Value::OPAQUE)
  {
    const enum_field_types ftyp= m_value.field_type();

    switch (ftyp)
    {
    case MYSQL_TYPE_NEWDECIMAL:
      return enum_json_type::J_DECIMAL;
    case MYSQL_TYPE_DATETIME:
      return enum_json_type::J_DATETIME;
    case MYSQL_TYPE_DATE:
      return enum_json_type::J_DATE;
    case MYSQL_TYPE_TIME:
      return enum_json_type::J_TIME;
    case MYSQL_TYPE_TIMESTAMP:
      return enum_json_type::J_TIMESTAMP;
    default: ;
      // ok, fall through
    }
  }

  return bjson2json(typ);
}


enum_field_types Json_wrapper::field_type() const
{
  if (m_is_dom)
  {
    return down_cast<Json_opaque *>(m_dom_value)->type();
  }

  return m_value.field_type();
}


Json_wrapper_object_iterator Json_wrapper::object_iterator() const
{
  DBUG_ASSERT(type() == enum_json_type::J_OBJECT);

  if (m_is_dom)
  {
    const Json_object *o= down_cast<const Json_object *>(m_dom_value);
    return Json_wrapper_object_iterator(o);
  }

  return Json_wrapper_object_iterator(&m_value);
}


Json_wrapper Json_wrapper::lookup(const std::string &key) const
{
  DBUG_ASSERT(type() == enum_json_type::J_OBJECT);
  if (m_is_dom)
  {
    const Json_object *object= down_cast<const Json_object *>(m_dom_value);
    Json_wrapper wr(object->get(key));
    wr.set_alias(); // wr doesn't own the supplied DOM: part of array DOM
    return wr;
  }

  return Json_wrapper(m_value.lookup(key));
}

Json_wrapper Json_wrapper::operator[](size_t index) const
{
  DBUG_ASSERT(type() == enum_json_type::J_ARRAY);
  if (m_is_dom)
  {
    const Json_array *o= down_cast<const Json_array *>(m_dom_value);
    Json_wrapper wr((*o)[index]);
    wr.set_alias(); // wr doesn't own the supplied DOM: part of array DOM
    return wr;
  }

  return Json_wrapper(m_value.element(index));
}


const char *Json_wrapper::get_data() const
{
  if (m_is_dom)
  {
    return type() == enum_json_type::J_STRING ?
      down_cast<Json_string *>(m_dom_value)->value().c_str() :
      down_cast<Json_opaque *>(m_dom_value)->value();
  }

  return m_value.get_data();
}


size_t Json_wrapper::get_data_length() const
{
  if (m_is_dom)
  {
    return type() == enum_json_type::J_STRING ?
      down_cast<Json_string *>(m_dom_value)->size() :
      down_cast<Json_opaque *>(m_dom_value)->size();
  }

  return m_value.get_data_length();
}


bool Json_wrapper::get_decimal_data(my_decimal *d) const
{
  if (m_is_dom)
  {
    *d= *down_cast<Json_decimal *>(m_dom_value)->value();
    return false;
  }

  return Json_decimal::convert_from_binary(m_value.get_data(),
                                           m_value.get_data_length(),
                                           d);
}


double Json_wrapper::get_double() const
{
  if (m_is_dom)
  {
    return down_cast<Json_double *>(m_dom_value)->value();
  }

  return m_value.get_double();
}


longlong Json_wrapper::get_int() const
{
  if (m_is_dom)
  {
    return down_cast<Json_int *>(m_dom_value)->value();
  }

  return m_value.get_int64();
}


ulonglong Json_wrapper::get_uint() const
{
  if (m_is_dom)
  {
    return down_cast<Json_uint *>(m_dom_value)->value();
  }

  return m_value.get_uint64();
}


void Json_wrapper::get_datetime(MYSQL_TIME *t) const
{
  enum_field_types ftyp= MYSQL_TYPE_NULL;

  switch(type())
  {
  case enum_json_type::J_DATE:
    ftyp= MYSQL_TYPE_DATE;
    break;
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_TIMESTAMP:
    ftyp= MYSQL_TYPE_DATETIME;
    break;
  case enum_json_type::J_TIME:
    ftyp= MYSQL_TYPE_TIME;
    break;
  default:
    DBUG_ASSERT(false);                         /* purecov: inspected */
  }

  if (m_is_dom)
  {
    *t= *down_cast<Json_datetime *>(m_dom_value)->value();
  }
  else
  {
    Json_datetime::from_packed(m_value.get_data(), ftyp, t);
  }
}


const char *Json_wrapper::get_datetime_packed(char *buffer) const
{
  if (m_is_dom)
  {
    down_cast<Json_datetime *>(m_dom_value)->to_packed(buffer);
    return buffer;
  }

  DBUG_ASSERT(m_value.get_data_length() == Json_datetime::PACKED_SIZE);
  return m_value.get_data();
}


bool Json_wrapper::get_boolean() const
{
  if (m_is_dom)
  {
    return down_cast<Json_boolean *>(m_dom_value)->value();
  }

  return m_value.type() == json_binary::Value::LITERAL_TRUE;
}


Json_path Json_dom::get_location()
{
  if (m_parent == NULL)
  {
    Json_path result;
    return result;
  }

  Json_path result= m_parent->get_location();

  if (m_parent->json_type() == enum_json_type::J_OBJECT)
  {
    Json_object *object= down_cast<Json_object *>(m_parent);
    for (Json_object::const_iterator it= object->begin();
         it != object->end(); ++it)
    {
      if (it->second == this)
      {
        Json_path_leg child_leg(it->first);
        result.append(child_leg);
        break;
      }
    }
  }
  else
  {
    DBUG_ASSERT(m_parent->json_type() == enum_json_type::J_ARRAY);
    Json_array *array= down_cast<Json_array *>(m_parent);

    for (size_t idx= 0; idx < array->size(); idx++)
    {
      if ((*array)[idx] == this)
      {
        Json_path_leg child_leg(idx);
        result.append(child_leg);
        break;
      }
    }
  }

  return result;
}


bool Json_dom::seek(const Json_seekable_path &path,
                    Json_dom_vector *hits,
                    bool auto_wrap, bool only_need_one)
{
  Json_dom_vector candidates(key_memory_JSON);
  Json_dom_vector duplicates(key_memory_JSON);

  if (hits->push_back(this))
    return true;                              /* purecov: inspected */

  size_t path_leg_count= path.leg_count();
  for (size_t path_idx= 0; path_idx < path_leg_count; path_idx++)
  {
    const Json_path_leg *path_leg= path.get_leg_at(path_idx);
    duplicates.clear();
    candidates.clear();

    for (Json_dom_vector::iterator it= hits->begin(); it != hits->end(); ++it)
    {
      if ((*it)->find_child_doms(path_leg, auto_wrap,
                                 (only_need_one &&
                                  (path_idx == (path_leg_count-1))),
                                 &duplicates, &candidates))
        return true;                          /* purecov: inspected */
    }

    // swap the two lists so that they can be re-used
    hits->swap(candidates);
  }

  return false;
}


bool Json_wrapper::seek_no_ellipsis(const Json_seekable_path &path,
                                    Json_wrapper_vector *hits,
                                    size_t current_leg,
                                    size_t last_leg,
                                    bool auto_wrap,
                                    bool only_need_one) const
{
  if (current_leg >= last_leg)
  {
    if (m_is_dom)
    {
      Json_wrapper clone(m_dom_value->clone());
      return clone.empty() || hits->push_back(std::move(clone));
    }
    return hits->push_back(*this);
  }

  const Json_path_leg *path_leg= path.get_leg_at(current_leg);
  const enum_json_type jtype= type();

  // Handle auto-wrapping of non-arrays.
  if (auto_wrap && jtype != enum_json_type::J_ARRAY && path_leg->is_autowrap())
  {
    // recursion
    return seek_no_ellipsis(path, hits, current_leg + 1, last_leg,
                            auto_wrap, only_need_one);
  }

  switch(path_leg->get_type())
  {
  case jpl_member:
    {
      switch (jtype)
      {
      case enum_json_type::J_OBJECT:
        {
          Json_wrapper member= lookup(path_leg->get_member_name());

          if (member.type() != enum_json_type::J_ERROR)
          {
            // recursion
            if (member.seek_no_ellipsis(path, hits, current_leg + 1, last_leg,
                                        auto_wrap, only_need_one))
              return true;                    /* purecov: inspected */
          }
          return false;
        }

      default:
        {
          return false;
        }
      } // end inner switch on wrapper type
    }

  case jpl_member_wildcard:
    {
      switch (jtype)
      {
      case enum_json_type::J_OBJECT:
        {
          for (Json_wrapper_object_iterator iter= object_iterator();
               !iter.empty(); iter.next())
          {
            if (is_seek_done(hits, only_need_one))
              return false;

            // recursion
            if (iter.elt().second.seek_no_ellipsis(path,
                                                   hits,
                                                   current_leg + 1,
                                                   last_leg,
                                                   auto_wrap,
                                                   only_need_one))
              return true;                    /* purecov: inspected */
          }
          return false;
        }

      default:
        {
          return false;
        }
      } // end inner switch on wrapper type
    }

  case jpl_array_cell:
    if (jtype == enum_json_type::J_ARRAY)
    {
      Json_array_index idx= path_leg->first_array_index(length());
      return idx.within_bounds() &&
        (*this)[idx.position()].seek_no_ellipsis(path, hits, current_leg + 1,
                                                 last_leg, auto_wrap,
                                                 only_need_one);
    }
    return false;

  case jpl_array_range:
  case jpl_array_cell_wildcard:
    if (jtype == enum_json_type::J_ARRAY)
    {
      auto range= path_leg->get_array_range(length());
      for (size_t idx= range.m_begin; idx < range.m_end; idx++)
      {
        if (is_seek_done(hits, only_need_one))
          return false;

        // recursion
        Json_wrapper cell= (*this)[idx];
        if (cell.seek_no_ellipsis(path, hits, current_leg + 1, last_leg,
                                  auto_wrap, only_need_one))
          return true;                        /* purecov: inspected */
      }
    }
    return false;

  default:
    // should never be called on a path which contains an ellipsis
    DBUG_ASSERT(false);                         /* purecov: inspected */
    return true;                                /* purecov: inspected */
  } // end outer switch on leg type
}


namespace
{

/// Does the path contain an ellipsis token?
bool contains_ellipsis(const Json_seekable_path &path)
{
  const size_t size= path.leg_count();
  for (size_t i= 0; i < size; i++)
    if (path.get_leg_at(i)->get_type() == jpl_ellipsis)
      return true;
  return false;
}

} // namespace


bool Json_wrapper::seek(const Json_seekable_path &path,
                        Json_wrapper_vector *hits,
                        bool auto_wrap, bool only_need_one)
{
  if (empty())
  {
    /* purecov: begin inspected */
    DBUG_ASSERT(false);
    return false;
    /* purecov: end */
  }

  // use fast-track code if the path doesn't have any ellipses
  if (!contains_ellipsis(path))
  {
    return seek_no_ellipsis(path, hits, 0, path.leg_count(),
                            auto_wrap, only_need_one);
  }

  /*
    FIXME.

    Materialize the dom if the path contains ellipses. Duplicate
    detection is difficult on binary values.
   */
  to_dom(current_thd);

  Json_dom_vector dhits(key_memory_JSON);
  if (m_dom_value->seek(path, &dhits, auto_wrap, only_need_one))
    return true;                              /* purecov: inspected */
  for (const Json_dom *dom : dhits)
  {
    Json_wrapper clone(dom->clone());
    if (clone.empty() || hits->push_back(std::move(clone)))
      return true;                            /* purecov: inspected */
  }

  return false;
}


size_t Json_wrapper::length() const
{
  if (empty())
  {
    return 0;
  }

  if (m_is_dom)
  {
    switch(m_dom_value->json_type())
    {
    case enum_json_type::J_ARRAY:
      return down_cast<Json_array *>(m_dom_value)->size();
    case enum_json_type::J_OBJECT:
      return down_cast<Json_object *>(m_dom_value)->cardinality();
    default:
      return 1;
    }
  }

  switch(m_value.type())
  {
  case json_binary::Value::ARRAY:
  case json_binary::Value::OBJECT:
    return m_value.element_count();
  default:
    return 1;
  }
}


size_t Json_wrapper::depth(const THD *thd) const
{
  if (empty())
  {
    return 0;
  }

  if (m_is_dom)
  {
    return m_dom_value->depth();
  }

  Json_dom *d= Json_dom::parse(thd, m_value);
  size_t result= d->depth();
  delete d;
  return result;
}


/**
  Compare two numbers of the same type.
  @param val1 the first number
  @param val2 the second number
  @retval -1 if val1 is less than val2,
  @retval 0 if val1 is equal to val2,
  @retval 1 if val1 is greater than val2
*/
template <class T> static int compare_numbers(T val1, T val2)
{
  return (val1 < val2) ? -1 : ((val1 == val2) ? 0 : 1);
}


/**
  Compare a decimal value to a double by converting the double to a
  decimal.
  @param a the decimal value
  @param b the double value
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_decimal_double(const my_decimal &a, double b)
{
  /*
    First check the sign of the two values. If they differ, the
    negative value is the smaller one.
  */
  const bool a_is_zero= my_decimal_is_zero(&a);
  const bool a_is_negative= a.sign() && !a_is_zero;
  const bool b_is_negative= (b < 0);
  if (a_is_negative != b_is_negative)
    return a_is_negative ? -1 : 1;

  // Both arguments have the same sign. Compare their values.

  const bool b_is_zero= b == 0;
  if (a_is_zero)
    // b is non-negative, so it is either equal to or greater than a.
    return b_is_zero ? 0 : -1;

  if (b_is_zero)
    // a is positive and non-zero, so it is greater than b.
    return 1;

  my_decimal b_dec;
  switch (double2decimal(b, &b_dec))
  {
  case E_DEC_OK:
    return my_decimal_cmp(&a, &b_dec);
  case E_DEC_OVERFLOW:
    /*
      b is too big to fit in a DECIMAL, so it must have a
      larger absolute value than a, which is a DECIMAL.
    */
    return a_is_negative ? 1 : -1;
  case E_DEC_TRUNCATED:
    /*
      b was truncated to fit in a DECIMAL, which means that b_dec is
      closer to zero than b.
    */
    {
      int cmp= my_decimal_cmp(&a, &b_dec);

      /*
        If the truncated b_dec is equal to a, a must be closer to zero
        than b.
      */
      if (cmp == 0)
        return a_is_negative ? 1 : -1;

      return cmp;
    }
  default:
    /*
      double2decimal() is not supposed to return anything other than
      E_DEC_OK, E_DEC_OVERFLOW or E_DEC_TRUNCATED, so this should
      never happen.
    */
    DBUG_ASSERT(false);                       /* purecov: inspected */
    return 1;                                 /* purecov: inspected */
  }
}


/**
  Compare a decimal value to a signed integer by converting the
  integer to a decimal.
  @param a the decimal value
  @param b the signed integer value
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_decimal_int(const my_decimal &a, longlong b)
{
  if (my_decimal_is_zero(&a))
    return (b == 0) ? 0 : (b > 0 ? -1 : 1);

  if (b == 0)
    return a.sign() ? -1 : 1;

  // Different signs. The negative number is the smallest one.
  if (a.sign() != (b < 0))
    return (b < 0) ? 1 : -1;

  // Couldn't tell the difference by looking at the signs. Compare as decimals.
  my_decimal b_dec;
  longlong2decimal(b, &b_dec);
  return my_decimal_cmp(&a, &b_dec);
}


/**
  Compare a decimal value to an unsigned integer by converting the
  integer to a decimal.
  @param a the decimal value
  @param b the unsigned integer value
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_decimal_uint(const my_decimal &a, ulonglong b)
{
  if (my_decimal_is_zero(&a))
    return (b == 0) ? 0 : -1;

  // If a is negative, it must be smaller than the unsigned value b.
  if (a.sign())
    return -1;

  // When we get here, we know that a is greater than zero.
  if (b == 0)
    return 1;

  // Couldn't tell the difference by looking at the signs. Compare as decimals.
  my_decimal b_dec;
  ulonglong2decimal(b, &b_dec);
  return my_decimal_cmp(&a, &b_dec);
}


/**
  Compare a JSON double to a JSON signed integer.
  @param a the double value
  @param b the integer value
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_double_int(double a, longlong b)
{
  double b_double= static_cast<double>(b);
  if (a < b_double)
    return -1;
  if (a > b_double)
    return 1;

  /*
    The two numbers were equal when compared as double. Since
    conversion from longlong to double isn't lossless, they could
    still be different. Convert to decimal to compare their exact
    values.
  */
  my_decimal b_dec;
  longlong2decimal(b, &b_dec);
  return -compare_json_decimal_double(b_dec, a);
}


/**
  Compare a JSON double to a JSON unsigned integer.
  @param a the double value
  @param b the unsigned integer value
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_double_uint(double a, ulonglong b)
{
  double b_double= ulonglong2double(b);
  if (a < b_double)
    return -1;
  if (a > b_double)
    return 1;

  /*
    The two numbers were equal when compared as double. Since
    conversion from longlong to double isn't lossless, they could
    still be different. Convert to decimal to compare their exact
    values.
  */
  my_decimal b_dec;
  ulonglong2decimal(b, &b_dec);
  return -compare_json_decimal_double(b_dec, a);
}


/**
  Compare a JSON signed integer to a JSON unsigned integer.
  @param a the signed integer
  @param b the unsigned integer
  @return -1 if a is less than b,
          0 if a is equal to b,
          1 if a is greater than b
*/
static int compare_json_int_uint(longlong a, ulonglong b)
{
  // All negative values are less than the unsigned value b.
  if (a < 0)
    return -1;

  // If a is not negative, it is safe to cast it to ulonglong.
  return compare_numbers(static_cast<ulonglong>(a), b);
}


/**
  Compare the contents of two strings in a JSON value. The strings
  could be either JSON string scalars encoded in utf8mb4, or binary
  strings from JSON opaque scalars. In either case they are compared
  byte by byte.

  @param str1 the first string
  @param str1_len the length of str1
  @param str2 the second string
  @param str2_len the length of str2
  @retval -1 if str1 is less than str2,
  @retval 0 if str1 is equal to str2,
  @retval 1 if str1 is greater than str2
*/
static int compare_json_strings(const char *str1, size_t str1_len,
                                const char *str2, size_t str2_len)
{
  int cmp= memcmp(str1, str2, std::min(str1_len, str2_len));
  if (cmp != 0)
    return cmp;
  return compare_numbers(str1_len, str2_len);
}

/// The number of enumerators in the enum_json_type enum.
static constexpr int num_json_types=
  static_cast<int>(enum_json_type::J_ERROR) + 1;

/**
  The following matrix tells how two JSON values should be compared
  based on their types. If type_comparison[type_of_a][type_of_b] is
  -1, it means that a is smaller than b. If it is 1, it means that a
  is greater than b. If it is 0, it means it cannot be determined
  which value is the greater one just by looking at the types.
*/
static constexpr int type_comparison[num_json_types][num_json_types]=
{
  /* NULL */      {0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* DECIMAL */   {1,  0,  0,  0,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* INT */       {1,  0,  0,  0,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* UINT */      {1,  0,  0,  0,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* DOUBLE */    {1,  0,  0,  0,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* STRING */    {1,  1,  1,  1,  1,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1},
  /* OBJECT */    {1,  1,  1,  1,  1,  1,  0, -1, -1, -1, -1, -1, -1, -1, -1},
  /* ARRAY */     {1,  1,  1,  1,  1,  1,  1,  0, -1, -1, -1, -1, -1, -1, -1},
  /* BOOLEAN */   {1,  1,  1,  1,  1,  1,  1,  1,  0, -1, -1, -1, -1, -1, -1},
  /* DATE */      {1,  1,  1,  1,  1,  1,  1,  1,  1,  0, -1, -1, -1, -1, -1},
  /* TIME */      {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0, -1, -1, -1, -1},
  /* DATETIME */  {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0, -1, -1},
  /* TIMESTAMP */ {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0, -1, -1},
  /* OPAQUE */    {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0, -1},
  /* ERROR */     {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1},
};


int Json_wrapper::compare(const Json_wrapper &other) const
{
  const enum_json_type this_type= type();
  const enum_json_type other_type= other.type();

  DBUG_ASSERT(this_type != enum_json_type::J_ERROR);
  DBUG_ASSERT(other_type != enum_json_type::J_ERROR);

  // Check if the type tells us which value is bigger.
  int cmp=
    type_comparison[static_cast<int>(this_type)][static_cast<int>(other_type)];
  if (cmp != 0)
    return cmp;

  // Same or similar type. Go on and inspect the values.

  switch (this_type)
  {
  case enum_json_type::J_ARRAY:
    /*
      Two arrays are equal if they have the same length, and all
      elements in one array are equal to the corresponding elements in
      the other array.

      The array that has the smallest value on the first position that
      contains different values in the two arrays, is considered
      smaller than the other array. If the two arrays are of different
      size, and all values in the shorter array are equal to the
      corresponding values in the longer array, the shorter array is
      considered smaller.
    */
    {
      const size_t size_a= length();
      const size_t size_b= other.length();
      const size_t min_size= std::min(size_a, size_b);
      for (size_t i= 0; i < min_size; i++)
      {
        int cmp= (*this)[i].compare(other[i]);
        if (cmp != 0)
          return cmp;
      }
      return compare_numbers(size_a, size_b);
    }
  case enum_json_type::J_OBJECT:
    /*
      An object is equal to another object if they have the same set
      of keys, and all values in one objects are equal to the values
      associated with the same key in the other object.
    */
    {
      /*
        If their sizes are different, the object with the smallest
        number of elements is smaller than the other object.
      */
      cmp= compare_numbers(length(), other.length());
      if (cmp != 0)
        return cmp;

      /*
        Otherwise, compare each key/value pair in the two objects.
        Return on the first difference that is found.
      */
      Json_wrapper_object_iterator it1= object_iterator();
      Json_wrapper_object_iterator it2= other.object_iterator();
      while (!it1.empty())
      {
        const std::pair<const std::string, Json_wrapper> elt1= it1.elt();
        const std::pair<const std::string, Json_wrapper> elt2= it2.elt();

        // Compare the keys of the two members.
        cmp= compare_json_strings(elt1.first.data(), elt1.first.size(),
                                  elt2.first.data(), elt2.first.size());
        if (cmp != 0)
          return cmp;

        // Compare the values of the two members.
        cmp= elt1.second.compare(elt2.second);
        if (cmp != 0)
          return cmp;

        it1.next();
        it2.next();
      }

      DBUG_ASSERT(it1.empty());
      DBUG_ASSERT(it2.empty());

      // No differences found. The two objects must be equal.
      return 0;
    }
  case enum_json_type::J_STRING:
    return compare_json_strings(get_data(), get_data_length(),
                                other.get_data(), other.get_data_length());
  case enum_json_type::J_INT:
    // Signed integers can be compared to all other numbers.
    switch (other_type)
    {
    case enum_json_type::J_INT:
      return compare_numbers(get_int(), other.get_int());
    case enum_json_type::J_UINT:
      return compare_json_int_uint(get_int(), other.get_uint());
    case enum_json_type::J_DOUBLE:
      return -compare_json_double_int(other.get_double(), get_int());
    case enum_json_type::J_DECIMAL:
      {
        my_decimal b_dec;
        if (other.get_decimal_data(&b_dec))
          return 1;                           /* purecov: inspected */
        return -compare_json_decimal_int(b_dec, get_int());
      }
    default:;
    }
    break;
  case enum_json_type::J_UINT:
    // Unsigned integers can be compared to all other numbers.
    switch (other_type)
    {
    case enum_json_type::J_UINT:
      return compare_numbers(get_uint(), other.get_uint());
    case enum_json_type::J_INT:
      return -compare_json_int_uint(other.get_int(), get_uint());
    case enum_json_type::J_DOUBLE:
      return -compare_json_double_uint(other.get_double(), get_uint());
    case enum_json_type::J_DECIMAL:
      {
        my_decimal b_dec;
        if (other.get_decimal_data(&b_dec))
          return 1;                           /* purecov: inspected */
        return -compare_json_decimal_uint(b_dec, get_uint());
      }
    default:;
    }
    break;
  case enum_json_type::J_DOUBLE:
    // Doubles can be compared to all other numbers.
    switch (other_type)
    {
    case enum_json_type::J_DOUBLE:
      return compare_numbers(get_double(), other.get_double());
    case enum_json_type::J_INT:
      return compare_json_double_int(get_double(), other.get_int());
    case enum_json_type::J_UINT:
      return compare_json_double_uint(get_double(), other.get_uint());
    case enum_json_type::J_DECIMAL:
      {
        my_decimal other_dec;
        if (other.get_decimal_data(&other_dec))
          return 1;                         /* purecov: inspected */
        return -compare_json_decimal_double(other_dec, get_double());
      }
    default:;
    }
    break;
  case enum_json_type::J_DECIMAL:
    // Decimals can be compared to all other numbers.
    {
      my_decimal a_dec;
      my_decimal b_dec;
      if (get_decimal_data(&a_dec))
        return 1;                             /* purecov: inspected */
      switch (other_type)
      {
      case enum_json_type::J_DECIMAL:
        if (other.get_decimal_data(&b_dec))
          return 1;                           /* purecov: inspected */
        /*
          my_decimal_cmp() treats -0 and 0 as not equal, so check for
          zero first.
        */
        if (my_decimal_is_zero(&a_dec) && my_decimal_is_zero(&b_dec))
          return 0;
        return my_decimal_cmp(&a_dec, &b_dec);
      case enum_json_type::J_INT:
        return compare_json_decimal_int(a_dec, other.get_int());
      case enum_json_type::J_UINT:
        return compare_json_decimal_uint(a_dec, other.get_uint());
      case enum_json_type::J_DOUBLE:
        return compare_json_decimal_double(a_dec, other.get_double());
      default:;
      }
      break;
    }
  case enum_json_type::J_BOOLEAN:
    // Booleans are only equal to other booleans. false is less than true.
    return compare_numbers(get_boolean(), other.get_boolean());
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_TIMESTAMP:
    // Timestamps and datetimes can be equal to each other.
    {
      MYSQL_TIME val_a;
      get_datetime(&val_a);
      MYSQL_TIME val_b;
      other.get_datetime(&val_b);
      return compare_numbers(TIME_to_longlong_packed(&val_a),
                             TIME_to_longlong_packed(&val_b));
    }
  case enum_json_type::J_TIME:
  case enum_json_type::J_DATE:
    // Dates and times can only be equal to values of the same type.
    {
      DBUG_ASSERT(this_type == other_type);
      MYSQL_TIME val_a;
      get_datetime(&val_a);
      MYSQL_TIME val_b;
      other.get_datetime(&val_b);
      return compare_numbers(TIME_to_longlong_packed(&val_a),
                             TIME_to_longlong_packed(&val_b));
    }
  case enum_json_type::J_OPAQUE:
    /*
      Opaque values are equal to other opaque values with the same
      field type and the same binary representation.
    */
    cmp= compare_numbers(field_type(), other.field_type());
    if (cmp == 0)
      cmp= compare_json_strings(get_data(), get_data_length(),
                                other.get_data(), other.get_data_length());
    return cmp;
  case enum_json_type::J_NULL:
    // Null is always equal to other nulls.
    DBUG_ASSERT(this_type == other_type);
    return 0;
  case enum_json_type::J_ERROR:
    break;
  }

  DBUG_ASSERT(false);                         /* purecov: inspected */
  return 1;                                   /* purecov: inspected */
}


/**
  Push a warning about a problem encountered when coercing a JSON
  value to some other data type.

  @param[in] target_type  the name of the target type of the coercion
  @param[in] error_code   the error code to use for the warning
  @param[in] msgnam       the name of the field/expression being coerced
*/
static void push_json_coercion_warning(const char *target_type,
                                       int error_code,
                                       const char *msgnam)
{
  /*
    One argument is no longer used (the empty string), but kept to avoid
    changing error message format.
  */
  push_warning_printf(current_thd,
                      Sql_condition::SL_WARNING,
                      error_code,
                      ER_THD(current_thd, error_code),
                      target_type,
                      "",
                      msgnam,
                      current_thd->get_stmt_da()->current_row_for_condition());
}


longlong Json_wrapper::coerce_int(const char *msgnam) const
{
  switch (type())
  {
  case enum_json_type::J_UINT:
    return static_cast<longlong>(get_uint());
  case enum_json_type::J_INT:
    return get_int();
  case enum_json_type::J_STRING:
    {
      /*
        For a string result, we must first get the string and then convert it
        to a longlong.
      */
      const char *start= get_data();
      size_t length= get_data_length();
      char *end= const_cast<char *>(start + length);
      const CHARSET_INFO *cs= &my_charset_utf8mb4_bin;

      int error;
      longlong value= cs->cset->strtoll10(cs, start, &end, &error);

      if (error > 0 || end != start + length)
      {
        int code= (error == MY_ERRNO_ERANGE ?
                   ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE :
                   ER_INVALID_JSON_VALUE_FOR_CAST);
        push_json_coercion_warning("INTEGER", code, msgnam);
      }

      return value;
    }
  case enum_json_type::J_BOOLEAN:
    return get_boolean() ? 1 : 0;
  case enum_json_type::J_DECIMAL:
    {
      longlong i;
      my_decimal decimal_value;
      get_decimal_data(&decimal_value);
      /*
        We do not know if this int is destined for signed or unsigned usage, so
        just get longlong from the value using the sign in the decimal.
      */
      my_decimal2int(E_DEC_FATAL_ERROR, &decimal_value, !decimal_value.sign(),
                     &i);
      return i;
    }
  case enum_json_type::J_DOUBLE:
    {
      // logic here is borrowed from Field_double::val_int
      double j= get_double();
      longlong res;

      if (j <= (double) LLONG_MIN)
      {
        res= LLONG_MIN;
      }
      else if (j >= (double) (ulonglong) LLONG_MAX)
      {
        res= LLONG_MAX;
      }
      else
      {
        return (longlong) rint(j);
      }

      push_json_coercion_warning("INTEGER",
                                 ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE, msgnam);
      return res;
    }
  default:;
  }

  push_json_coercion_warning("INTEGER", ER_INVALID_JSON_VALUE_FOR_CAST,
                             msgnam);
  return 0;
}


double Json_wrapper::coerce_real(const char *msgnam) const
{
  switch (type())
  {
  case enum_json_type::J_DECIMAL:
    {
      double dbl;
      my_decimal decimal_value;
      get_decimal_data(&decimal_value);
      my_decimal2double(E_DEC_FATAL_ERROR, &decimal_value, &dbl);
      return dbl;
    }
  case enum_json_type::J_STRING:
    {
      /*
        For a string result, we must first get the string and then convert it
        to a double.
      */
      const char *start= get_data();
      size_t length= get_data_length();
      char *end= const_cast<char*>(start) + length;
      const CHARSET_INFO *cs= &my_charset_utf8mb4_bin;

      int error;
      double value= my_strntod(cs, const_cast<char*>(start), length,
                               &end, &error);

      if (error || end != start + length)
      {
        int code= (error == EOVERFLOW ?
                   ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE :
                   ER_INVALID_JSON_VALUE_FOR_CAST);
        push_json_coercion_warning("DOUBLE", code, msgnam);
      }
      return value;
    }
  case enum_json_type::J_DOUBLE:
    return get_double();
  case enum_json_type::J_INT:
    return static_cast<double>(get_int());
  case enum_json_type::J_UINT:
    return static_cast<double>(get_uint());
  case enum_json_type::J_BOOLEAN:
    return static_cast<double>(get_boolean());
  default:;
  }

  push_json_coercion_warning("DOUBLE", ER_INVALID_JSON_VALUE_FOR_CAST,
                             msgnam);
  return 0.0;
}

my_decimal
*Json_wrapper::coerce_decimal(my_decimal *decimal_value,
                              const char *msgnam) const
{
  switch (type())
  {
  case enum_json_type::J_DECIMAL:
    get_decimal_data(decimal_value);
    return decimal_value;
  case enum_json_type::J_STRING:
    {
      /*
        For a string result, we must first get the string and then convert it
        to a decimal.
      */
      // has own error handling, but not very informative
      int err= str2my_decimal(E_DEC_FATAL_ERROR, get_data(), get_data_length(),
                              &my_charset_utf8mb4_bin, decimal_value);
      if (err)
      {
        int code= (err == E_DEC_OVERFLOW ?
                   ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE :
                   ER_INVALID_JSON_VALUE_FOR_CAST);
        push_json_coercion_warning("DECIMAL", code, msgnam);
      }
      return decimal_value;
    }
  case enum_json_type::J_DOUBLE:
    if (double2my_decimal(E_DEC_FATAL_ERROR, get_double(), decimal_value))
    {
      push_json_coercion_warning("DECIMAL",
                                 ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE, msgnam);
    }
    return decimal_value;
  case enum_json_type::J_INT:
    if (longlong2decimal(get_int(), decimal_value))
    {
      push_json_coercion_warning("DECIMAL",
                                 ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE, msgnam);
    }
    return decimal_value;
  case enum_json_type::J_UINT:
    if (longlong2decimal(get_uint(), decimal_value))
    {
      push_json_coercion_warning("DECIMAL",
                                 ER_NUMERIC_JSON_VALUE_OUT_OF_RANGE, msgnam);
    }
    return decimal_value;
  case enum_json_type::J_BOOLEAN:
    // no danger of overflow, so void result
    (void)int2my_decimal(E_DEC_FATAL_ERROR, get_boolean(),
                         true /* unsigned */, decimal_value);
    return decimal_value;
  default:;
  }

  push_json_coercion_warning("DECIMAL", ER_INVALID_JSON_VALUE_FOR_CAST,
                             msgnam);

  my_decimal_set_zero(decimal_value);
  return decimal_value;
}


bool Json_wrapper::coerce_date(MYSQL_TIME *ltime,
                               const char *msgnam) const
{
  bool result= coerce_time(ltime, msgnam);

  if (!result && ltime->time_type == MYSQL_TIMESTAMP_TIME)
  {
    MYSQL_TIME tmp= *ltime;
    time_to_datetime(current_thd, &tmp, ltime);
  }

  return result;
}


bool Json_wrapper::coerce_time(MYSQL_TIME *ltime,
                               const char *msgnam) const
{
  switch (type())
  {
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_DATE:
  case enum_json_type::J_TIME:
  case enum_json_type::J_TIMESTAMP:
    set_zero_time(ltime, MYSQL_TIMESTAMP_DATETIME);
    get_datetime(ltime);
    return false;
  default:
    push_json_coercion_warning("DATE/TIME/DATETIME/TIMESTAMP",
                               ER_INVALID_JSON_VALUE_FOR_CAST, msgnam);
    return true;
  }
}


namespace
{

/// Wrapper around a sort key buffer.
class Wrapper_sort_key
{
private:
  uchar *m_buffer;  ///< the buffer into which to write
  size_t m_length;  ///< the length of the buffer
  size_t m_pos;     ///< the current position in the buffer

public:
  Wrapper_sort_key(uchar *buf, size_t len)
    : m_buffer(buf), m_length(len), m_pos(0)
  {}

  /// Get the remaining space in the buffer.
  size_t remaining() const
  {
    return m_length - m_pos;
  }

  /// Get how much space we've used so far.
  size_t pos() const
  {
    return m_pos;
  }

  /// Append a character to the buffer.
  void append(uchar ch)
  {
    if (m_pos < m_length)
      m_buffer[m_pos++]= ch;
  }

 /**
    Pad the buffer with the specified character till given position.
    @note This function is intended to be used to make numbers of equal length
    without occupying the whole buffer.
  */
  void pad_till(uchar pad_character, size_t pos)
  {
    longlong num_chars= pos - m_pos;
    DBUG_ASSERT(num_chars >= 0);
    num_chars= std::min(remaining(), static_cast<size_t>(num_chars));
    memset(m_buffer + m_pos, pad_character, num_chars);
    m_pos += num_chars;
  }

  /**
    Copy an integer to the buffer and format it in a way that makes it
    possible to sort the integers with memcpy().

    @param target_length  the number of bytes to write to the buffer
    @param from           the buffer to copy the integer from (in little-endian
                          format)
    @param from_length    the size of the from buffer
    @param is_unsigned    true if the from buffer contains an unsigned integer,
                          false otherwise
  */
  void copy_int(size_t target_length,
                const uchar* from, size_t from_length,
                bool is_unsigned)
  {
    size_t to_length= std::min(remaining(), target_length);
    copy_integer<false>(m_buffer + m_pos, to_length,
                        from, from_length, is_unsigned);
    m_pos+= to_length;
  }

  /**
    Append a string to the buffer, and add the length of the string to
    the end of the buffer. The space between the end of the string and
    the beginning of the length field is padded with zeros.
  */
  void append_str_and_len(const char *str, size_t len)
  {
    /*
      The length is written as a four byte value at the end of the
      buffer, provided that there is enough room and string to be stored is
      longer than buffer.
    */
    size_t space_for_len= (len <= remaining()) ? 0 :
                          std::min(static_cast<size_t>(VARLEN_PREFIX),
                          remaining());

    /*
      The string contents are written up to where the length is
      stored, and get truncated if the string is longer than that.
    */
    size_t space_for_str= remaining() - space_for_len;
    size_t copy_len= std::min(len, space_for_str);
    memcpy(m_buffer + m_pos, str, copy_len);
    m_pos+= copy_len;

    /*
      Write the length in a format that memcmp() knows how to sort.
      First we store it in little-endian format in a four-byte buffer,
      and then we use copy_integer to transform it into a format that
      works with memcmp().
    */
    if (space_for_str)
    {
      uchar length_buffer[VARLEN_PREFIX];
      int4store(length_buffer, static_cast<uint32>(len));
      copy_int(space_for_len,
               length_buffer, sizeof(length_buffer), true);
    }
  }
};


/// Helper class for building a hash key.
class Wrapper_hash_key
{
private:

  ulonglong m_crc;

public:

  Wrapper_hash_key(ulonglong *hash_val) : m_crc(*hash_val)
  {}

  /**
    Return the computed hash value.
  */
  ulonglong get_crc()
  {
    return m_crc;
  }

  void add_character(uchar ch)
  {
    add_to_crc(ch);
  }

  void add_integer(longlong ll)
  {
    char tmp[8];
    int8store(tmp, ll);
    add_string(tmp, sizeof(tmp));
  }

  void add_double(double d)
  {
    // Make -0.0 and +0.0 have the same key.
    if (d == 0)
    {
      add_character(0);
      return;
    }

    char tmp[8];
    float8store(tmp, d);
    add_string(tmp, sizeof(tmp));
  }

  void add_string(const char *str, size_t len)
  {
    for (size_t idx= 0; idx < len; idx++)
    {
      add_to_crc(*str++);
    }
  }

private:

  /**
    Add another character to the evolving crc.

    @param[in] ch The character to add
  */
  void add_to_crc(uchar ch)
  {
    // This logic was cribbed from sql_executor.cc/unique_hash
    m_crc=((m_crc << 8) +
         (((uchar) ch))) +
      (m_crc >> (8*sizeof(ha_checksum)-8));
  }
};


/*
  Type identifiers used in the sort key generated by
  Json_wrapper::make_sort_key(). Types with lower identifiers sort
  before types with higher identifiers.
  See also note for Json_dom::enum_json_type.
*/
constexpr uchar JSON_KEY_NULL=        '\x00';
constexpr uchar JSON_KEY_NUMBER_NEG=  '\x01';
constexpr uchar JSON_KEY_NUMBER_ZERO= '\x02';
constexpr uchar JSON_KEY_NUMBER_POS=  '\x03';
constexpr uchar JSON_KEY_STRING=      '\x04';
constexpr uchar JSON_KEY_OBJECT=      '\x05';
constexpr uchar JSON_KEY_ARRAY=       '\x06';
constexpr uchar JSON_KEY_FALSE=       '\x07';
constexpr uchar JSON_KEY_TRUE=        '\x08';
constexpr uchar JSON_KEY_DATE=        '\x09';
constexpr uchar JSON_KEY_TIME=        '\x0A';
constexpr uchar JSON_KEY_DATETIME=    '\x0B';
constexpr uchar JSON_KEY_OPAQUE=      '\x0C';

} // namespace

/*
  Max char position to pad numeric sort keys to. Includes max precision +
  sort key len.
*/
#define MAX_NUMBER_SORT_PAD \
  (std::max(DBL_DIG, DECIMAL_MAX_POSSIBLE_PRECISION) + VARLEN_PREFIX + 3)

/**
  Make a sort key for a JSON numeric value from its string representation. The
  input string could be either on scientific format (such as 1.234e2) or on
  plain format (such as 12.34).

  The sort key will have the following parts:

  1) One byte that is JSON_KEY_NUMBER_NEG, JSON_KEY_NUMBER_ZERO or
  JSON_KEY_NUMBER_POS if the number is positive, zero or negative,
  respectively.

  2) Two bytes that represent the decimal exponent of the number (log10 of the
  number, truncated to an integer).

  3) All the digits of the number, without leading zeros.

  4) Padding to ensure that equal numbers sort equal even if they have a
  different number of trailing zeros.

  If the number is zero, parts 2, 3 and 4 are skipped.

  For negative numbers, the values in parts 2, 3 and 4 need to be inverted so
  that bigger negative numbers sort before smaller negative numbers.

  @param[in]     from     the string representation of the number
  @param[in]     len      the length of the input string
  @param[in]     negative true if the number is negative, false otherwise
  @param[in,out] to       the target sort key
*/
static void make_json_numeric_sort_key(const char *from, size_t len,
                                       bool negative, Wrapper_sort_key *to)
{
  const char *end= from + len;

  // Find the start of the exponent part, if there is one.
  const char *end_of_digits= std::find(from, end, 'e');

  /*
    Find the first significant digit. Skip past sign, leading zeros
    and the decimal point, until the first non-zero digit is found.
  */
  const auto is_non_zero_digit= [] (char c) { return c >= '1' && c <= '9'; };
  const char *first_significant_digit=
    std::find_if(from, end_of_digits, is_non_zero_digit);

  if (first_significant_digit == end_of_digits)
  {
    // We didn't find any significant digits, so the number is zero.
    to->append(JSON_KEY_NUMBER_ZERO);
    return;
  }

  longlong exp;
  if (end_of_digits != end)
  {
    // Scientific format. Fetch the exponent part after the 'e'.
    char *endp= const_cast<char *>(end);
    exp= my_strtoll(end_of_digits + 1, &endp, 10);
  }
  else
  {
    /*
      Otherwise, find the exponent by calculating the distance between the
      first significant digit and the decimal point.
    */
    const char *dec_point= std::find(from, end_of_digits, '.');
    if (!dec_point)
    {
      // There is no decimal point. Just count the digits.
      exp= end_of_digits - first_significant_digit - 1;
    }
    else if (first_significant_digit < dec_point)
    {
      // Non-negative exponent.
      exp= dec_point - first_significant_digit - 1;
    }
    else
    {
      // Negative exponent.
      exp= dec_point - first_significant_digit;
    }
  }

  if (negative)
  {
    to->append(JSON_KEY_NUMBER_NEG);
    /*
      For negative numbers, we have to invert the exponents so that numbers
      with high exponents sort before numbers with low exponents.
    */
    exp= -exp;
  }
  else
  {
    to->append(JSON_KEY_NUMBER_POS);
  }

  /*
    Store the exponent part before the digits. Since the decimal exponent of a
    double can be in the range [-323, +308], we use two bytes for the
    exponent. (Decimals and bigints also fit in that range.)
  */
  uchar exp_buff[2];
  int2store(exp_buff, static_cast<int16>(exp));
  to->copy_int(sizeof(exp_buff), exp_buff, sizeof(exp_buff), false);

  /*
    Append all the significant digits of the number. Stop before the exponent
    part if there is one, otherwise go to the end of the string.
  */
  for (const char *ch= first_significant_digit; ch < end_of_digits; ++ch)
  {
    if (my_isdigit(&my_charset_numeric, *ch))
    {
      /*
        If the number is negative, the digits must be inverted so that big
        negative numbers sort before small negative numbers.
      */
      if (negative)
        to->append('9' - *ch + '0');
      else
        to->append(*ch);
    }
  }

  /*
    Pad the number with zeros up to 30 bytes, so that the number of trailing
    zeros doesn't affect how the number is sorted. As above, we need to invert
    the digits for negative numbers.
  */
  to->pad_till(negative ? '9' : '0', MAX_NUMBER_SORT_PAD);
}


size_t Json_wrapper::make_sort_key(uchar *to, size_t to_length) const
{
  Wrapper_sort_key key(to, to_length);

  const enum_json_type jtype= type();
  switch (jtype)
  {
  case enum_json_type::J_NULL:
    key.append(JSON_KEY_NULL);
    break;
  case enum_json_type::J_DECIMAL:
    {
      my_decimal dec;
      if (get_decimal_data(&dec))
        break;                                  /* purecov: inspected */
      char buff[DECIMAL_MAX_STR_LENGTH + 1];
      String str(buff, sizeof(buff), &my_charset_numeric);
      if (my_decimal2string(E_DEC_FATAL_ERROR, &dec, 0, 0, 0, &str))
        break;                                  /* purecov: inspected */
      make_json_numeric_sort_key(str.ptr(), str.length(), dec.sign(), &key);
      break;
    }
  case enum_json_type::J_INT:
    {
      longlong i= get_int();
      char buff[MAX_BIGINT_WIDTH + 1];
      size_t len= longlong10_to_str(i, buff, -10) - buff;
      make_json_numeric_sort_key(buff, len, i < 0, &key);
      break;
    }
  case enum_json_type::J_UINT:
    {
      ulonglong ui= get_uint();
      char buff[MAX_BIGINT_WIDTH + 1];
      size_t len= longlong10_to_str(ui, buff, 10) - buff;
      make_json_numeric_sort_key(buff, len, false, &key);
      break;
    }
  case enum_json_type::J_DOUBLE:
    {
      double dbl= get_double();
      char buff[MY_GCVT_MAX_FIELD_WIDTH + 1];
      size_t len= my_gcvt(dbl, MY_GCVT_ARG_DOUBLE,
                          sizeof(buff) - 1, buff, NULL);
      make_json_numeric_sort_key(buff, len, (dbl < 0), &key);
      break;
    }
  case enum_json_type::J_STRING:
    key.append(JSON_KEY_STRING);
    key.append_str_and_len(get_data(), get_data_length());
    break;
  case enum_json_type::J_OBJECT:
  case enum_json_type::J_ARRAY:
    /*
      Internal ordering of objects and arrays only considers length
      for now.
    */
    {
      key.append(jtype == enum_json_type::J_OBJECT ?
                 JSON_KEY_OBJECT : JSON_KEY_ARRAY);
      uchar len[4];
      int4store(len, static_cast<uint32>(length()));
      key.copy_int(sizeof(len), len, sizeof(len), true);
      /*
        Raise a warning to give an indication that sorting of objects
        and arrays is not properly supported yet. The warning is
        raised for each object/array that is found during the sort,
        but Filesort_error_handler will make sure that only one
        warning is seen on the top level for every sort.
      */
      push_warning_printf(current_thd, Sql_condition::SL_WARNING,
                          ER_NOT_SUPPORTED_YET,
                          ER_THD(current_thd, ER_NOT_SUPPORTED_YET),
                          "sorting of non-scalar JSON values");
      break;
    }
  case enum_json_type::J_BOOLEAN:
    key.append(get_boolean() ? JSON_KEY_TRUE : JSON_KEY_FALSE);
    break;
  case enum_json_type::J_DATE:
  case enum_json_type::J_TIME:
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_TIMESTAMP:
    {
      if (jtype == enum_json_type::J_DATE)
        key.append(JSON_KEY_DATE);
      else if (jtype == enum_json_type::J_TIME)
        key.append(JSON_KEY_TIME);
      else
        key.append(JSON_KEY_DATETIME);

      /*
        Temporal values are stored in the packed format in the binary
        JSON format. The packed values are 64-bit signed little-endian
        integers.
      */
      const size_t packed_length= Json_datetime::PACKED_SIZE;
      char tmp[packed_length];
      const char *packed= get_datetime_packed(tmp);
      key.copy_int(packed_length,
                   pointer_cast<const uchar *>(packed),
                   packed_length, false);
      break;
    }
  case enum_json_type::J_OPAQUE:
    key.append(JSON_KEY_OPAQUE);
    key.append(field_type());
    key.append_str_and_len(get_data(), get_data_length());
    break;
  case enum_json_type::J_ERROR:
    break;
  }

  return key.pos();
}


ulonglong Json_wrapper::make_hash_key(ulonglong *hash_val)
{
  Wrapper_hash_key hash_key(hash_val);
  switch (type())
  {
  case enum_json_type::J_NULL:
    hash_key.add_character(JSON_KEY_NULL);
    break;
  case enum_json_type::J_DECIMAL:
    {
      my_decimal dec;
      if (get_decimal_data(&dec))
        break;                                  /* purecov: inspected */
      double dbl;
      decimal2double(&dec, &dbl);
      hash_key.add_double(dbl);
      break;
    }
  case enum_json_type::J_INT:
    hash_key.add_double(static_cast<double>(get_int()));
    break;
  case enum_json_type::J_UINT:
    hash_key.add_double(ulonglong2double(get_uint()));
    break;
  case enum_json_type::J_DOUBLE:
    hash_key.add_double(get_double());
    break;
  case enum_json_type::J_STRING:
  case enum_json_type::J_OPAQUE:
    hash_key.add_string(get_data(), get_data_length());
    break;
  case enum_json_type::J_OBJECT:
    {
      hash_key.add_character(JSON_KEY_OBJECT);
      for (Json_wrapper_object_iterator it(object_iterator());
           !it.empty(); it.next())
      {
        std::pair<const std::string, Json_wrapper> pair= it.elt();
        hash_key.add_string(pair.first.c_str(), pair.first.length());
        ulonglong t= hash_key.get_crc();
        hash_key.add_integer(pair.second.make_hash_key(&t));
      }
      break;
    }
  case enum_json_type::J_ARRAY:
    {
      hash_key.add_character(JSON_KEY_ARRAY);
      size_t elts= length();
      for (uint i= 0; i < elts; i++)
      {
        ulonglong t= hash_key.get_crc();
        hash_key.add_integer((*this)[i].make_hash_key(&t));
      }
    break;
    }
  case enum_json_type::J_BOOLEAN:
    hash_key.add_character(get_boolean() ? JSON_KEY_TRUE : JSON_KEY_FALSE);
    break;
  case enum_json_type::J_DATE:
  case enum_json_type::J_TIME:
  case enum_json_type::J_DATETIME:
  case enum_json_type::J_TIMESTAMP:
    {
      const size_t packed_length= Json_datetime::PACKED_SIZE;
      char tmp[packed_length];
      const char *packed= get_datetime_packed(tmp);
      hash_key.add_string(packed, packed_length);
      break;
    }
  case enum_json_type::J_ERROR:
    DBUG_ASSERT(false);                         /* purecov: inspected */
    break;                                      /* purecov: inspected */
  }

  ulonglong result= hash_key.get_crc();
  return result;
}


bool Json_wrapper::get_free_space(size_t *space) const
{
  if (m_is_dom)
  {
    *space= 0;
    return false;
  }

  return m_value.get_free_space(current_thd, space);
}


bool Json_wrapper::attempt_binary_update(const Field_json *field,
                                         const Json_seekable_path &path,
                                         Json_wrapper *new_value,
                                         bool replace,
                                         String *result,
                                         bool *partially_updated,
                                         bool *replaced_path)
{
  using namespace json_binary;

  // Can only do partial update if the input value is binary.
  DBUG_ASSERT(!is_dom());

  /*
    If we are replacing the top-level document, there's no need for
    partial update. The full document is rewritten anyway.
  */
  if (path.leg_count() == 0)
  {
    *partially_updated= false;
    *replaced_path= false;
    return false;
  }

  // Find the parent of the value we want to modify.
  Json_wrapper_vector hits(key_memory_JSON);
  if (seek_no_ellipsis(path, &hits, 0, path.leg_count() - 1, false, true))
    return true;                                /* purecov: inspected */

  if (hits.empty())
  {
    /*
      No parent array/object was found, so both JSON_SET and
      JSON_REPLACE will be no-ops. Return success.
    */
    *partially_updated= true;
    *replaced_path= false;
    return false;
  }

  DBUG_ASSERT(hits.size() == 1);
  DBUG_ASSERT(!hits[0].is_dom());

  auto &parent= hits[0].m_value;
  const Json_path_leg *last_leg= path.get_leg_at(path.leg_count() - 1);
  size_t element_pos;
  switch (parent.type())
  {
  case Value::OBJECT:
    if (last_leg->get_type() != jpl_member)
    {
      /*
        Nothing to do for JSON_REPLACE, because we cannot replace an
        array cell in an object. JSON_SET will auto-wrap the object,
        so fall back to full update in that case
      */
      *partially_updated= replace;
      *replaced_path= false;
      return false;
    }
    element_pos= parent.lookup_index(last_leg->get_member_name());
    /*
      If the member is not found, JSON_REPLACE is done (it's a no-op),
      whereas JSON_SET will need to add a new element to the object.
    */
    if (element_pos == parent.element_count())
    {
      *partially_updated= replace;
      *replaced_path= false;
      return false;
    }
    break;
  case Value::ARRAY:
    {
      if (last_leg->get_type() != jpl_array_cell)
      {
        // Nothing to do. Cannot replace an object member in an array.
        *partially_updated= true;
        *replaced_path= false;
        return false;
      }
      Json_array_index idx= last_leg->first_array_index(parent.element_count());
      /*
        If the element is not found, JSON_REPLACE is done (it's a no-op),
        whereas JSON_SET will need to add a new element to the array
      */
      if (!idx.within_bounds())
      {
        *partially_updated= replace;
        *replaced_path= false;
        return false;
      }
      element_pos= idx.position();
    }
    break;
  default:
    /*
      There's no element to replace inside a scalar, so we're done if
      we have replace semantics. JSON_SET may want to auto-wrap the
      scalar if it is accessed as an array, and in that case we need
      to fall back to full update.
    */
    *partially_updated= replace || (last_leg->get_type() != jpl_array_cell);
    *replaced_path= false;
    return false;
  }

  DBUG_ASSERT(element_pos < parent.element_count());

  // Find out how much space we need to store new_value.
  size_t needed;
  const THD *thd= field->table->in_use;
  if (space_needed(thd, new_value, parent.large_format(), &needed))
    return true;

  // Do we have that space available?
  size_t data_offset= 0;
  if (needed > 0 && !parent.has_space(element_pos, needed, &data_offset))
  {
    *partially_updated= false;
    *replaced_path= false;
    return false;
  }

  /*
    Get a pointer to the binary representation of the document. If the result
    buffer is not empty, it contains the binary representation of the document,
    including any other partial updates made to it previously in this
    operation. If it is empty, the document is unchanged and its binary
    representation can be retrieved from the Field.
  */
  const char *original;
  if (result->is_empty())
  {
    if (m_value.raw_binary(thd, result))
      return true;                              /* purecov: inspected */
    original= field->get_binary();
  }
  else
  {
    DBUG_ASSERT(is_binary_backed_by(result));
    original= result->ptr();
  }

  DBUG_ASSERT(result->length() >= data_offset + needed);

  char *destination= const_cast<char *>(result->ptr());
  if (parent.update_in_shadow(field, element_pos, new_value, data_offset,
                              needed, original, destination))
    return true;                                /* purecov: inspected */

  m_value= parse_binary(result->ptr(), result->length());
  *partially_updated= true;
  *replaced_path= true;
  return false;
}


bool Json_wrapper::binary_remove(const Field_json *field,
                                 const Json_seekable_path &path,
                                 String *result, bool *found_path)
{
  // Can only do partial update if the input value is binary.
  DBUG_ASSERT(!is_dom());

  // Empty paths are short-circuited higher up. (Should be a no-op.)
  DBUG_ASSERT(path.leg_count() > 0);

  *found_path= false;

  Json_wrapper_vector hits(key_memory_JSON);
  if (seek_no_ellipsis(path, &hits, 0, path.leg_count() - 1, false, true))
    return true;                                /* purecov: inspected */

  DBUG_ASSERT(hits.size() <= 1);

  if (hits.empty())
    return false;

  auto &parent= hits[0].m_value;
  const Json_path_leg *last_leg= path.get_leg_at(path.leg_count() - 1);
  size_t element_pos;
  switch (parent.type())
  {
  case json_binary::Value::OBJECT:
    if (last_leg->get_type() != enum_json_path_leg_type::jpl_member)
      return false;   // no match, nothing to remove
    element_pos= parent.lookup_index(last_leg->get_member_name());
    break;
  case json_binary::Value::ARRAY:
    {
      if (last_leg->get_type() != enum_json_path_leg_type::jpl_array_cell)
        return false;   // no match, nothing to remove
      Json_array_index idx= last_leg->first_array_index(parent.element_count());
      if (!idx.within_bounds())
        return false;  // no match, nothing to remove
      element_pos= idx.position();
      break;
    }
  default:
    // Can only remove elements from objects and arrays, so nothing to do.
    return false;
  }

  if (element_pos >= parent.element_count())
    return false;    // no match, nothing to remove

  /*
    Get a pointer to the binary representation of the document. If the result
    buffer is not empty, it contains the binary representation of the document,
    including any other partial updates made to it previously in this
    operation. If it is empty, the document is unchanged and its binary
    representation can be retrieved from the Field.
  */
  const char *original;
  if (result->is_empty())
  {
    if (m_value.raw_binary(field->table->in_use, result))
      return true;                              /* purecov: inspected */
    original= field->get_binary();
  }
  else
  {
    DBUG_ASSERT(is_binary_backed_by(result));
    original= result->ptr();
  }

  char *destination= const_cast<char *>(result->ptr());

  if (parent.remove_in_shadow(field, element_pos, original, destination))
    return true;                                /* purecov: inspected */

  m_value= json_binary::parse_binary(result->ptr(), result->length());
  *found_path= true;
  return false;
}
