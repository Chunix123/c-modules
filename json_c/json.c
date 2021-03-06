#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdalign.h>

#include "../common/z_type.h"
#include "json.h"

//#define offsetof(s, m)   (size_t)&(((s *)0)->m)

#ifndef alignof
	// Define alignof for C99 compatibility
	// Credit to Martin Buchholz from: http://www.wambold.com/Martin/writings/alignof.html
	#define alignof(type) offsetof (struct { char c; type member; }, member)
#endif

#define CONSOLE_RED "\x1B[31m"
#define CONSOLE_RESET "\x1B[0m"


typedef struct Mempool
{
	u_int8_t *start;
	u_int8_t *end;
	u_int8_t *top;
} Mempool;

Mempool buffer = { .end = NULL, .top = NULL };

void Json_set_mempool(void *start, size_t size)
{
	buffer.start = start;
	buffer.top = start;
	buffer.end = buffer.start + size;
}

void Json_reset_mempool()
{
	buffer.top = buffer.start;
}

void * _json_alloc(size_t size, size_t alignment) 
{
	if (!buffer.end)
	{
		printf("Mempool not allocated.\n");
		return NULL;
	}

	// Alignment
	// Check to see if the current top is aligned.
	int padding = 0;
	int remainder = (size_t) buffer.top % alignment;
	if (remainder != 0)
	{
		padding = alignment - remainder;
		buffer.top += padding;
	}

	void * loc = (void *) buffer.top;
	buffer.top += size;

	if (buffer.top >= buffer.end)
	{
		printf("Out of memory!\n");
		loc = NULL;
	}

	#ifdef DEBUG_JSON
	printf("Requested: %lu ", size);
	printf("Alignment: %lu ", alignment);
	printf("Padding: %d ", padding);
	printf("Free bytes: %lu ", (size_t)(buffer.end - buffer.top + 1));
	printf("Top: %p\n", (void *)buffer.top);
	#endif

	return loc;
}

const unsigned char DEFAULT_LETTER = 0x80;
const u_int16_t DEFAULT_OBJECT_ADDRESS = 0xFFFF;
void _set_default_JsonNode(JsonNode* node)
{
	// 1000 0000
	// Since Ascii characters are only 7 bits long, the 
	// first bit can act as a "flag" when a node is unitialized.
	node->sibling = DEFAULT_OBJECT_ADDRESS;
	node->child = DEFAULT_OBJECT_ADDRESS;
	node->data = DEFAULT_OBJECT_ADDRESS;
	node->letter = DEFAULT_LETTER; // 1000 0000.
}

// Creates an empty JSON object, equivalent of {}
JsonObject* create_JsonObject()
{
	JsonNode node;
	_set_default_JsonNode(&node);
	JsonObject* obj = _json_alloc(sizeof(JsonObject), alignof(JsonObject));
	obj->node = node;

	return obj;
}

JsonValue get_value(JsonObject * obj, char * key)
{
	JsonNode * node = &(obj->node);

	if (!(*key))
	{
		while (*key != node->letter)
		{
			node = (JsonNode*)(buffer.start + node->sibling);

			if ((u_int8_t *) node - buffer.start == DEFAULT_OBJECT_ADDRESS)
			{
				return (JsonValue) {
					.type=JSON_ERROR, 
					.data.e=MISSING_KEY
				};
			}
		}
	}

	while (*key)
	{
		while (*key != node->letter)
		{
			node = (JsonNode*)(buffer.start + node->sibling);
			if ((u_int8_t *) node - buffer.start == DEFAULT_OBJECT_ADDRESS)
			{
				return (JsonValue) {
					.type=JSON_ERROR, 
					.data.e=MISSING_KEY
				};
			}
		}

		node = (JsonNode*)(buffer.start + node->child);
		if ((u_int8_t *) node - buffer.start == DEFAULT_OBJECT_ADDRESS)
		{
			return (JsonValue) {
				.type=JSON_ERROR, 
				.data.e=MISSING_KEY
			};
		}
		key++;
	}

	if (node->data != DEFAULT_OBJECT_ADDRESS)
	{
		return *((JsonValue*)(buffer.start + node->data));
	}
	else
	{
		return (JsonValue) {
			.type=JSON_ERROR, 
			.data.e=MISSING_KEY
		};
	}
}

int _alloc_JsonElement(JsonValue * jd, void * data)
{
	switch (jd->type)
	{
		case JSON_NULL:
			jd->data.n = NULL;
			break;
		case JSON_STRING:
		{
			char * destination = _json_alloc(strlen((char *) data) + 1, alignof(char));
			strcpy(destination, (char *) data);
			jd->data.s = destination;
			break;
		}
		case JSON_BOOL:
			jd->data.b = *((bool *) data);
			break;
		case JSON_FLOAT:
			jd->data.f = *((float *) data);
			break;
		case JSON_OBJECT:
			jd->data.o = (JsonObject *) data;
			break;
		case JSON_ARRAY:
		{
			jd->data.a = (JsonArray *) data;
			break;
		}
		default:
			return INVALID_TYPE; 
	}

	return 0;
}

bool _set_value(JsonObject * obj, char * key, void* data, JsonDataType type)
{
	JsonValue* value = _json_alloc(sizeof(JsonValue), alignof(JsonValue));
	value->type = type;

	int status = _alloc_JsonElement(value, data);
	if (status < 0)
	{
		return false;
	}

	JsonNode * node = &(obj->node);
	// Check if the JSON node is set to its default values. If that is the case,
	// we can save an extra allocation by chaning the default value's key rather
	// than by creating a sibling.
	if (node->letter & DEFAULT_LETTER)
	{
		_set_default_JsonNode(node);
		node->letter = *key;
	}

	if (!(*key))
	{
		while (*key != node->letter)
		{
			if (node->sibling == DEFAULT_OBJECT_ADDRESS)
			{
				JsonNode * sibling = _json_alloc(sizeof(JsonNode), alignof(JsonNode));
				_set_default_JsonNode(sibling);
				sibling->letter = *key;
				node->sibling = ((u_int8_t *) sibling - buffer.start);
			}
			node = (JsonNode*)(buffer.start + node->sibling);
		}
	}

	// If passed in an empty string, go directly to setting the object's value.
	while (*key)
	{
		// Otherwise traverse sideways until a matching letter is found
		while (*key != node->letter)
		{
			if (node->sibling == DEFAULT_OBJECT_ADDRESS)
			{
				JsonNode * sibling = _json_alloc(sizeof(JsonNode), alignof(JsonNode));
				_set_default_JsonNode(sibling);
				sibling->letter = *key;
				node->sibling = ((u_int8_t *) sibling - buffer.start);
			}
			node = (JsonNode*)(buffer.start + node->sibling);
		}

		// Check if the next character is null terminating.
		// If it is, then the current node will be the one to which data is
		// added to. If node, then traverse one node down, creating a new node
		// if it does not already exist.
		key++;
		if (node->child == DEFAULT_OBJECT_ADDRESS)
		{
			JsonNode * child = _json_alloc(sizeof(JsonNode), alignof(JsonNode));
			_set_default_JsonNode(child);
			child->letter = *key;
			node->child = ((u_int8_t *) child - buffer.start);
		}
		node = (JsonNode*)(buffer.start + node->child);
	}

	node->data = ((u_int8_t *) value - buffer.start);
	return true;
}

bool set_value_null(JsonObject * obj, char * key)
{
	return _set_value(obj, key, NULL, JSON_NULL);
}

bool set_value_string(JsonObject * obj, char * key, char * str)
{
	return _set_value(obj, key, str, JSON_STRING);
}

bool set_value_bool(JsonObject * obj, char * key, bool data)
{
	return _set_value(obj, key, &data, JSON_BOOL);
}

bool set_value_float(JsonObject * obj, char * key, float data)
{
	return _set_value(obj, key, &data, JSON_FLOAT);
}

bool set_value_object(JsonObject * obj, char * key, JsonObject * object)
{
	return _set_value(obj, key, object, JSON_OBJECT);
}

bool set_value_array(JsonObject * obj, char * key, JsonArray * array)
{
	return _set_value(obj, key, array, JSON_ARRAY);
}

JsonArray * create_JsonArray(u_int16_t length)
{
	JsonArray* j = _json_alloc(sizeof(JsonArray), alignof(JsonArray));
	j->length = length;

	JsonValue * elements = _json_alloc(sizeof(JsonValue) * length, alignof(JsonArray));
	j->elements = ((u_int8_t *) elements - buffer.start);
	return j;
}

int _set_element(JsonArray * j, u_int16_t index, void * data, JsonDataType type)
{
	JsonValue *jd = &(((JsonValue*)(buffer.start + j->elements))[index]);
	jd->type = type;
	int status = _alloc_JsonElement(jd, data);
	return status;
}

bool set_element_null(JsonArray * j, u_int16_t index)
{
	return _set_element(j, index, NULL, JSON_NULL);
}

bool set_element_string(JsonArray * j, u_int16_t index, char * str)
{
	return _set_element(j, index, str, JSON_STRING);
}

bool set_element_bool(JsonArray * j, u_int16_t index, bool data)
{
	return _set_element(j, index, &data, JSON_BOOL);
}

bool set_element_float(JsonArray * j, u_int16_t index, float data)
{
	return _set_element(j, index, &data, JSON_FLOAT);
}

bool set_element_object(JsonArray * j, u_int16_t index, JsonObject * object)
{
	return _set_element(j, index, object, JSON_OBJECT);
}

bool set_element_array(JsonArray * j, u_int16_t index, JsonArray * array)
{
	return _set_element(j, index, array, JSON_ARRAY);
}

JsonValue get_element(JsonArray * j, u_int16_t index)
{
	if (index > j->length)
	{
		return (JsonValue) {
			.type=JSON_ERROR, 
			.data.e=INDEX_OUT_OF_BOUNDS
		};
	}
	return ((JsonValue*)(buffer.start + j->elements))[index];
}

#define JSON_STACK_LENGTH 128
typedef struct _Stack
{
	void* stack[JSON_STACK_LENGTH];
	int stacktop;
} _Stack;

int push_ptr(_Stack* s, void* p)
{
	if (s->stacktop >= JSON_STACK_LENGTH - 1)
	{
		printf("Json: Stack overflow\n");
		return -1;
	}

	(s->stacktop)++;
	s->stack[s->stacktop] = p;

	return 0;
}

int push_int(_Stack* s, int i)
{
	if (s->stacktop >= JSON_STACK_LENGTH - 1)
	{
		printf("Json: Stack overflow\n");
		return -1;
	}

	(s->stacktop)++;
	((int *)s->stack)[s->stacktop] = i;

	return 0;
}

void* pop_ptr(_Stack* s)
{
	void *rval = s->stack[s->stacktop];
	(s->stacktop)--;

	return rval;
}

void* peek_ptr(_Stack* s)
{
	void *rval = s->stack[s->stacktop];
	return rval;
}

int pop_int(_Stack* s)
{
	int rval = ((int*)s->stack)[s->stacktop];
	(s->stacktop)--;

	return rval;
}

int peek_int(_Stack* s)
{
	int rval = ((int*)s->stack)[s->stacktop];
	return rval;
}

typedef struct _Dumper
{
	_Stack valstack;
	_Stack bufend_stack;
	_Stack objIndex_stack;
	_Stack dump_stack;
	char * destination;
	char * key_buffer;
} _Dumper;

enum JsonDumpTypes
{
	Dump_JsonValue,
	Dump_JsonObject,
	Dump_JsonArray,
	Dump_JsonObject_Key
};

char _JSON_NULL_STR[] = "null";
char _JSON_FALSE_STR[] = "false";
char _JSON_TRUE_STR[] = "true";

void _dump_JsonObject(JsonObject *o, _Dumper* dumper);
void _dump_JsonValue(JsonValue *value, _Dumper* dumper);
void _dump_JsonArray(JsonArray *ary, _Dumper* dumper);
void _dump_JsonObject_Key(_Dumper * dumper, int bufStart, int bufEnd);

void _dump_JsonObject_Key(_Dumper * dumper, int bufStart, int bufEnd)
{
	*(dumper->destination++) = '"'; 
	for (int i = bufStart; i <= bufEnd; i++)
	{
		*(dumper->destination++) = dumper->key_buffer[i];
	}
	*(dumper->destination++) = '"'; 
	*(dumper->destination++) = ':'; 
}

void _dump_JsonArray(JsonArray *ary, _Dumper* dumper)
{
	*(dumper->destination++) = '[';
	for (int i = 0; i < ary->length; i++)
	{
		if (i > 0)
		{
			*(dumper->destination++) = ',';
		}

		JsonValue* element = &((JsonValue*)(buffer.start + ary->elements))[i];
		switch (element->type)
		{
			case JSON_OBJECT:
				_dump_JsonObject(element->data.o, dumper);
				break;
			default:
				_dump_JsonValue(element, dumper);
				break;
		}
	}
	*(dumper->destination++) = ']';
}

void _dump_JsonValue(JsonValue* value, _Dumper* dumper)
{
	switch (value->type)
	{
		char *str;
		case JSON_NULL:
			str = _JSON_NULL_STR;
			while (*str) *(dumper->destination++) = *(str++);
			break;
		case JSON_STRING:
			str = value->data.s;
			*(dumper->destination++) = '"';
			while (*str) *(dumper->destination++) = *(str++);
			*(dumper->destination++) = '"';
			break;
		case JSON_BOOL:
			str = value->data.b ? _JSON_TRUE_STR : _JSON_FALSE_STR;
			while (*str) *(dumper->destination++) = *(str++);
			break;
		case JSON_FLOAT:
			dumper->destination += sprintf(dumper->destination, "%g", value->data.f);
			break;
		case JSON_OBJECT:
			push_int(&dumper->objIndex_stack, dumper->valstack.stacktop);
			push_ptr(&dumper->valstack, &(value->data.o->node));
			push_int(&dumper->bufend_stack, 0);
			*(dumper->destination++) = '{';
			break;
		case JSON_ARRAY:
			_dump_JsonArray(value->data.a, dumper);
			break;
		default:
			break;
	}
}

void _dump_JsonObject(JsonObject *o, _Dumper * dumper)
{
	push_int(&dumper->objIndex_stack, dumper->valstack.stacktop);
	push_ptr(&dumper->valstack, &(o->node));
	push_int(&dumper->bufend_stack, 0);

	int intial_valstack_top = dumper->valstack.stacktop;
	int intial_objIndex_stack_top = dumper->objIndex_stack.stacktop;

	*(dumper->destination++) = '{';
	while (dumper->valstack.stacktop >= intial_valstack_top)
	{
		JsonNode* node = pop_ptr(&dumper->valstack);
		int strIndex  = pop_int(&dumper->bufend_stack);  // This will update strIndex
		dumper->key_buffer[strIndex] = node->letter;  // Add the current key to the buffer

		// Add sibling to stack if exists
		if (node->sibling != DEFAULT_OBJECT_ADDRESS)
		{
			JsonNode* sibling = (JsonNode*)(buffer.start + node->sibling); 
			push_ptr(&dumper->valstack, sibling);
			push_int(&dumper->bufend_stack, strIndex);
		}

		// Add child to stack if exists
		if (node->child != DEFAULT_OBJECT_ADDRESS)
		{
			JsonNode* child = (JsonNode*)(buffer.start + node->child); 
			push_ptr(&dumper->valstack, child);
			push_int(&dumper->bufend_stack, strIndex + 1);
		}

		// Print current node if it's not empty
		if (node->data != DEFAULT_OBJECT_ADDRESS)
		{
			if (*(dumper->destination - 1) != '{')
			{
				*(dumper->destination++) = ',';
			}

			_dump_JsonObject_Key(dumper, 0, strIndex - 1 );
			JsonValue* value = (JsonValue*)(buffer.start + node->data);
			_dump_JsonValue(value, dumper);
		}

		if (dumper->valstack.stacktop == peek_int(&dumper->objIndex_stack))
		{
			*(dumper->destination++) = '}';
			pop_int(&dumper->objIndex_stack);
		}
	}

	// Check this at the end, since nested objects will not check for the '}'
	while (dumper->objIndex_stack.stacktop >= intial_objIndex_stack_top)
	{
		*(dumper->destination++) = '}';
		pop_int(&dumper->objIndex_stack);
	}
}

size_t dump_JsonObject(JsonObject* o, char* destination)
{
	char key_buffer[256];
	_Dumper dumper;
	dumper.key_buffer = key_buffer;
	dumper.destination = destination;
	dumper.valstack.stacktop = -1;
	dumper.bufend_stack.stacktop = -1;
	dumper.objIndex_stack.stacktop = -1;

	_dump_JsonObject(o, &dumper);
	*(dumper.destination) = '\0';
	
	return dumper.destination - destination;
}

typedef struct _Parser
{
	char* input;
	char* buffer;
	JsonValue* arrayBuffer;
	_Stack jsonParseStack;
	_Stack jsonObjectStack;
	_Stack jsonBufferStack;
	_Stack jsonDeserializeStack;
} _Parser;

enum JsonParseTypes
{
	Parse_JsonObjectStart,
	Parse_JsonMembers,
	Parse_JsonElements,
	Parse_JsonValue,
	Parse_Colon,
	Parse_JsonValueSeparator,
	Parse_JsonElementSeparator,
	Parse_JsonString,
	Parse_JsonNumber,
};

// Create the jump table for the parser
bool parse_JsonObjectStart(_Parser *);
bool parse_JsonMembers(_Parser *);
bool parse_JsonElements(_Parser *);
bool parse_JsonValue(_Parser *);
bool parse_Colon(_Parser *);
bool parse_JsonValueSeparator(_Parser *);
bool parse_JsonElementSeparator(_Parser *);
bool parse_JsonString(_Parser *);
bool parse_JsonNumber(_Parser *);
bool (*_parser_jump_table[9])(_Parser*) =
{
	&parse_JsonObjectStart,
	&parse_JsonMembers,
	&parse_JsonElements,
	&parse_JsonValue,
	&parse_Colon,
	&parse_JsonValueSeparator,
	&parse_JsonElementSeparator,
	&parse_JsonString,
	&parse_JsonNumber,
};

enum JsonDeserializeTypes
{
	Deserialize_JsonObject,
	Deserialize_JsonArray,
};

void next_token(_Parser* p)
{
	#ifdef DEBUG_JSON
	printf("Current token: '%c'\n", *p->input);
	#endif
	p->input++;
}

void skip_whitespace(_Parser* parser)
{
	while (*(parser->input))
	{
		switch (*(parser->input))
		{
			case ' ':
			case '\r':
			case '\t':
			case '\n':
			case '\v':
				break;
			default:
				return;
		}
		next_token(parser);
	}
}

bool parse_JsonObjectStart(_Parser* parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing object start\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case '{':
			pop_int(&parser->jsonParseStack);
			push_int(&parser->jsonParseStack, Parse_JsonMembers);
			push_ptr(&parser->jsonObjectStack, create_JsonObject());
			push_int(&parser->jsonDeserializeStack, Deserialize_JsonObject);
			next_token(parser);
			return true;
		default:
			return false;
	}
	next_token(parser);
}

bool parse_JsonElements(_Parser* parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing elements\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case ']':
			pop_int(&parser->jsonParseStack);
			pop_int(&parser->jsonDeserializeStack);
			JsonValue * lastElement = parser->arrayBuffer;
			JsonValue * firstElement = pop_ptr(&parser->jsonObjectStack);
			JsonArray * array = create_JsonArray(lastElement - firstElement);
			for (int i = lastElement - firstElement - 1; i >= 0; i--)
			{
				JsonValue element = firstElement[i];
				switch (element.type)
				{
					case JSON_STRING:
						_set_element(array, i, element.data.s, element.type);
						// Strings are stored in the parser's buffer, so they need to popped.
						parser->buffer = pop_ptr(&parser->jsonBufferStack);
						break;
					case JSON_OBJECT:
						_set_element(array, i, element.data.o, element.type);
						break;
					case JSON_ARRAY:
						_set_element(array, i, element.data.a, element.type);
						break;
					default:
						_set_element(array, i, &element.data, element.type);
						break;
				}
			}
			parser->arrayBuffer = firstElement;

			enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
			if (type == Deserialize_JsonObject)
			{
				JsonObject* parent = peek_ptr(&parser->jsonObjectStack);
				parser->buffer = pop_ptr(&parser->jsonBufferStack);
				_set_value(parent, parser->buffer, array, JSON_ARRAY);
			}
			else if (type == Deserialize_JsonArray)
			{
				JsonValue * element = parser->arrayBuffer++;
				element->type = JSON_ARRAY;
				element->data.a = array;
			}

			next_token(parser);
			return true;
		default:
			push_int(&parser->jsonParseStack, Parse_JsonElementSeparator);
			push_int(&parser->jsonParseStack, Parse_JsonValue);
			return true;
	}
}

bool parse_JsonMembers(_Parser* parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing members\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case '}':
			pop_int(&parser->jsonDeserializeStack);
			pop_int(&parser->jsonParseStack);
			if (parser->jsonObjectStack.stacktop > 0)
			{
				JsonObject* child = pop_ptr(&parser->jsonObjectStack);
				enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
				if (type == Deserialize_JsonObject)
				{
					JsonObject* parent = peek_ptr(&parser->jsonObjectStack);
					parser->buffer = pop_ptr(&parser->jsonBufferStack);
					_set_value(parent, parser->buffer, child, JSON_OBJECT);
				}
				else if (type == Deserialize_JsonArray)
				{
					JsonValue * element = parser->arrayBuffer++;
					element->type = JSON_OBJECT;
					element->data.o = child;
				}
			}
			next_token(parser);
			return true;
		case '"':
			push_int(&parser->jsonParseStack, Parse_JsonValueSeparator);
			push_int(&parser->jsonParseStack, Parse_JsonValue);
			push_int(&parser->jsonParseStack, Parse_Colon);
			push_int(&parser->jsonParseStack, Parse_JsonString);
			next_token(parser);
			return true;
		default:
			return false;
	}
}

bool parse_EscapedChar(_Parser * parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing escaped character\n");
	#endif
	next_token(parser);
	switch (*(parser->input))
	{
		case '"':
		*(parser->buffer++) = '\"';
			break;
		case '\\':
			*(parser->buffer++) = '\\';
			break;
		case '/':
			*(parser->buffer++) = '/';
			break;
		case 'b':
			*(parser->buffer++) = '\b';
			break;
		case 'f':
			*(parser->buffer++) = '\f';
			break;
		case 'n':
			*(parser->buffer++) = '\n';
			break;
		case 'r':
			*(parser->buffer++) = '\r';
			break;
		case 't':
			*(parser->buffer++) = '\t';
			break;
		default:
			return false;
	}

	return true;
}

bool parse_JsonString(_Parser * parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing string\n");
	#endif
	push_ptr(&parser->jsonBufferStack, parser->buffer);
	while (*(parser->input))
	{
		switch (*(parser->input))
		{
			case '"':
				*(parser->buffer++) = '\0';
				pop_int(&parser->jsonParseStack);
				next_token(parser);
				return true;
			case '\\':
				if (!parse_EscapedChar(parser))
				{
					return false;
				}
				break;
			default:
				*(parser->buffer++) = *(parser->input);
		}
		next_token(parser);
	}

	return true;
}

bool parse_Colon(_Parser * parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing colon\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case ':':
			pop_int(&parser->jsonParseStack);
			next_token(parser);
			break;
		default:
			return false;
	}

	return true;
}

bool parse_JsonValue(_Parser * parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing json value\n");
	#endif
	skip_whitespace(parser);
	pop_int(&parser->jsonParseStack);
	switch (*(parser->input))
	{
		case '"':
			next_token(parser);
			push_int(&parser->jsonParseStack, Parse_JsonString);
			parse_JsonString(parser);

			enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
			if (type == Deserialize_JsonObject)
			{
				char* value = pop_ptr(&parser->jsonBufferStack);
				parser->buffer = pop_ptr(&parser->jsonBufferStack);
				JsonObject * o = peek_ptr(&parser->jsonObjectStack);
				_set_value(o, parser->buffer, value, JSON_STRING);
			}
			else if (type == Deserialize_JsonArray)
			{
				// With arrays, the string is kept in the buffer. They will need to later be
				// removed when the elemeent is added to the string.
				char * value = peek_ptr(&parser->jsonBufferStack);
				JsonValue * element = parser->arrayBuffer++;
				element->type = JSON_STRING;
				element->data.s = value;
			}
			break;
		case 'n':
		{
			for (unsigned long i = 0; i < sizeof(_JSON_NULL_STR) - 1; i++)
			{
				if (*(parser->input) != _JSON_NULL_STR[i]) return false;
				next_token(parser);
			}

			enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
			if (type == Deserialize_JsonObject)
			{
				JsonObject * o = peek_ptr(&parser->jsonObjectStack);
				parser->buffer = pop_ptr(&parser->jsonBufferStack);
				_set_value(o, parser->buffer, NULL, JSON_NULL);
			}
			else if (type == Deserialize_JsonArray)
			{
				JsonValue *element = parser->arrayBuffer++;
				element->type = JSON_NULL;
				element->data.n = NULL;
			}
			break;
		}
		case 't':
		{
			for (unsigned long i = 0; i < sizeof(_JSON_TRUE_STR) - 1; i++)
			{
				if (*(parser->input) != _JSON_TRUE_STR[i]) return false;
				next_token(parser);
			}

			enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
			if (type == Deserialize_JsonObject)
			{
				JsonObject *o = peek_ptr(&parser->jsonObjectStack);
				parser->buffer = pop_ptr(&parser->jsonBufferStack);
				bool temp = true;
				_set_value(o, parser->buffer, &temp, JSON_BOOL);
			}
			else if (type == Deserialize_JsonArray)
			{
				JsonValue *element = parser->arrayBuffer++;
				element->type = JSON_BOOL;
				element->data.b = true;
			}
			break;
		}
		case 'f':
		{
			for (unsigned long i = 0; i < sizeof(_JSON_FALSE_STR) - 1; i++)
			{
				if (*(parser->input) != _JSON_FALSE_STR[i]) return false;
				next_token(parser);
			}

			enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
			if (type == Deserialize_JsonObject)
			{
				JsonObject * o = peek_ptr(&parser->jsonObjectStack);
				parser->buffer = pop_ptr(&parser->jsonBufferStack);
				bool temp = false;
				_set_value(o, parser->buffer, &temp, JSON_BOOL);
			}
			else if (type == Deserialize_JsonArray)
			{
				JsonValue *element = parser->arrayBuffer++;
				element->type = JSON_BOOL;
				element->data.b = false;
			}
			break;
		}
		case '0': case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9': case '-':
			push_int(&parser->jsonParseStack, Parse_JsonNumber);
			break;
		case '{':
			push_int(&parser->jsonParseStack, Parse_JsonObjectStart);
			break;
		case '[':
			push_int(&parser->jsonParseStack, Parse_JsonElements);
			push_int(&parser->jsonDeserializeStack, Deserialize_JsonArray);
			push_ptr(&parser->jsonObjectStack, parser->arrayBuffer);
			next_token(parser);
			break;
		default:
			return false;
	}

	return true;
}

bool parse_JsonValueSeparator(_Parser* parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing json value separator\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case '}':
			pop_int(&parser->jsonParseStack);
			return true;
		case ',':
			next_token(parser);
			pop_int(&parser->jsonParseStack);
			return true;
		default:
			return false;
	}
}

bool parse_JsonElementSeparator(_Parser* parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing json element separator\n");
	#endif
	skip_whitespace(parser);
	switch (*(parser->input))
	{
		case ']':
			pop_int(&parser->jsonParseStack);
			return true;
		case ',':
			next_token(parser);
			pop_int(&parser->jsonParseStack);
			return true;
		default:
			return false;
	}
}

bool parse_JsonNumber(_Parser *parser)
{
	#ifdef DEBUG_JSON
	printf("Parsing json number\n");
	#endif
	// Parse the number using strtod
	char * start = parser->input;
	char * end = parser->input;
	float val = strtod(start, &end);
	parser->input = end;

	enum JsonDeserializeTypes type = peek_int(&parser->jsonDeserializeStack);
	if (type == Deserialize_JsonObject)
	{
		JsonObject *o = peek_ptr(&parser->jsonObjectStack);
		parser->buffer = pop_ptr(&parser->jsonBufferStack);
		_set_value(o, parser->buffer, &val, JSON_FLOAT);
	}
	else if (type == Deserialize_JsonArray)
	{
		JsonValue* element = parser->arrayBuffer++;
		element->type = JSON_FLOAT;
		element->data.f = val;
	}

	if (start == end)
	{
		next_token(parser);
		return false;
	}

	pop_int(&parser->jsonParseStack);
	return true;
}

void print_error(_Parser * parser, char * input)
{
	const int inputLength = 50, maxLeadingChars = 40;
	char *start = start = input < parser->input - maxLeadingChars ? parser->input - maxLeadingChars : input;

	char erroneousInput[inputLength + 1];
	strncpy(erroneousInput, start, inputLength);
	erroneousInput[inputLength] = '\0';

	char invalidTokenArrow[inputLength];
	int badIndex = parser->input - start;
	for (int i = 0; i < inputLength; i++)
	{
		if (i < badIndex)
		{
			invalidTokenArrow[i] = ' ';
		}
		else if (i == badIndex)
		{
			invalidTokenArrow[i] = '^';
		}
		else
		{
			invalidTokenArrow[i] = '\0';
			break;
		}
	}

	if (*(parser->input))
	{
		char bad_char = *(parser->input);
		printf(
			CONSOLE_RED "Invalid token found at position %li: '%c' (%d)\n" CONSOLE_RESET,
			parser->input - input,
			bad_char, 
			(int)bad_char);
	}
	else
	{
		printf(CONSOLE_RED "Unexpected end of input");
	}
	printf(CONSOLE_RED "%s\n" CONSOLE_RESET, erroneousInput);
	printf(CONSOLE_RED "%s\n" CONSOLE_RESET, invalidTokenArrow);
}

bool parse_JsonObject(char* input, JsonObject** parsed)
{
	*parsed = NULL;
	char buffer[1024];
	JsonValue arrayBuffer[1024];
	_Parser parser;
	parser.input = input;
	parser.buffer = buffer;
	parser.arrayBuffer = arrayBuffer;
	parser.jsonParseStack.stacktop = -1;
	parser.jsonObjectStack.stacktop = -1;
	parser.jsonBufferStack.stacktop = -1;
	parser.jsonDeserializeStack.stacktop = -1;

	// Skip leading whitespace
	skip_whitespace(&parser);

	// Expect to start parsing an object.
	push_int(&parser.jsonParseStack, Parse_JsonObjectStart);
	while (parser.jsonParseStack.stacktop >= 0)
	{
		bool success = _parser_jump_table[peek_int(&parser.jsonParseStack)](&parser);
		if (!success)
		{
			print_error(&parser, input);
			return false;
		}
	}

	*parsed = pop_ptr(&parser.jsonObjectStack);

	#ifdef DEBUG_JSON
	printf("%d\n", parser.jsonParseStack.stacktop);
	printf("%d\n", parser.jsonObjectStack.stacktop);
	printf("%d\n", parser.jsonBufferStack.stacktop);
	printf("%d\n", parser.jsonDeserializeStack.stacktop);
	printf("%li\n", parser.buffer - buffer);
	printf("%li\n", parser.arrayBuffer - arrayBuffer);
	#endif

	return true;
}
