#pragma once

#define EXPORT_PRIVATE_MEMBER_IMPL(field,member,disc) \
namespace priv_ ## disc { \
	template<typename T,auto M> \
	struct Steal{ friend constexpr decltype(auto) steal_ ## disc(T){return M;} }; \
	struct Key{ friend constexpr decltype(auto) steal_ ## disc(Key); }; \
	template struct Steal<Key,member>; \
} static constexpr decltype(auto) field = steal_ ## disc(priv_ ## disc :: Key())

#define EXPORT_PRIVATE_MEMBER_X(field,member,fc) EXPORT_PRIVATE_MEMBER_IMPL(field,member,fc)

#define EXPORT_PRIVATE_MEMBER(new_name,member) EXPORT_PRIVATE_MEMBER_X(new_name,member,__COUNTER__)
