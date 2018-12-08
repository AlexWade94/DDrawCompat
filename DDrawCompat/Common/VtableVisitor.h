#pragma once

#include <ddraw.h>
#include <typeinfo>

template <typename Vtable>
struct VtableForEach;

template <typename Vtable, typename Visitor>
void forEach(Visitor& visitor)
{
	VtableForEach<Vtable>::forEach<Vtable>(visitor);
}

template <typename T>
std::string getTypeName()
{
	std::string typeName(typeid(T).name());
	if (0 == typeName.find("struct "))
	{
		typeName = typeName.substr(typeName.find(" ") + 1);
	}
	return typeName;
}

#ifdef DEBUGLOGS
#define DD_VISIT(member) \
		visitor.visitDebug<decltype(&Vtable::member), &Vtable::member>(getTypeName<Vtable>(), #member)
#else
#define DD_VISIT(member) \
		visitor.visit<decltype(&Vtable::member), &Vtable::member>()
#endif

template <>
struct VtableForEach<IUnknownVtbl>
{
	template <typename Vtable, typename Visitor>
	static void forEach(Visitor& visitor)
	{
		DD_VISIT(QueryInterface);
		DD_VISIT(AddRef);
		DD_VISIT(Release);
	}
};
