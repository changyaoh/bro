/*
Copyright (C) 2013 Jarryd Beck (adapted by Matthias Vallentin, then Jon Siwek).

Distributed under the Boost Software License, Version 1.0

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

  The copyright notices in the Software and this entire statement, including
  the above license grant, this restriction and the following disclaimer,
  must be included in all copies of the Software, in whole or in part, and
  all derivative works of the Software, unless such copies or derivative
  works are solely in the form of machine-executable object code generated by
  a source language processor.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
  SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
  FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
*/

#ifndef BROKER_UTIL_VARIANT_HH
#define BROKER_UTIL_VARIANT_HH

#include <cassert>
#include <type_traits>
#include <memory>
#include <functional>
#include <broker/util/operators.hh>
#include <broker/util/meta.hh>

namespace broker {
namespace util {

namespace detail {

template <typename T>
struct getter {
	using result_type = T*;

	result_type operator()(T& val) const
		{ return &val; }

	template <typename U>
	result_type operator()(const U& u) const
		{ return nullptr; }
};

struct hasher {
	using result_type = size_t;

	template <typename T>
	result_type operator()(const T& x) const
		{ return std::hash<T>{}(x); }
};

template <typename Visitor>
class delayed_visitor {
public:

	using result_type = typename remove_reference_t<Visitor>::result_type;

	delayed_visitor(Visitor v)
		: visitor(std::move(v))
		{}

	template <typename... Visitables>
	result_type operator()(Visitables&&... vs)
		{ return apply_visitor(visitor, std::forward<Visitables>(vs)...); }

private:

	Visitor visitor;
};

template <typename Visitor>
class delayed_visitor_wrapper {
public:

	using result_type = typename remove_reference_t<Visitor>::result_type;

	delayed_visitor_wrapper(Visitor& visitor)
		: visitor(visitor)
		{}

	template <typename... Visitables>
	result_type operator()(Visitables&&... vs)
		{ return apply_visitor(visitor, std::forward<Visitables>(vs)...); }

private:

	Visitor& visitor;
};

template <typename Visitor, typename Visitable>
class binary_visitor {
public:

	using result_type = typename remove_reference_t<Visitor>::result_type;

	binary_visitor(Visitor& arg_visitor, Visitable& arg_visitable)
		: visitor(arg_visitor), visitable(arg_visitable)
		{}

	template <typename... Ts>
	result_type operator()(Ts&&... xs)
		{
		return visitable.template
		       apply<std::false_type>(visitor, std::forward<Ts>(xs)...);
		}

private:

	Visitor& visitor;
	Visitable& visitable;
};

template <typename T>
class recursive_wrapper : totally_ordered<recursive_wrapper<T>> {
public:

	template <typename U,
	          typename Dummy = enable_if_t<std::is_convertible<U, T>{}, U>>
	recursive_wrapper(const U& u)
		: item(new T(u))
		{ }

	template <typename U,
	          typename Dummy = enable_if_t<std::is_convertible<U, T>{}, U>>
	recursive_wrapper(U&& u)
		: item(new T(std::forward<U>(u)))
		{ }

	recursive_wrapper(const recursive_wrapper& other)
		: item(new T(other.get()))
		{ }

	recursive_wrapper(recursive_wrapper&& other) noexcept
		: item(other.item.release())
		{ }

	recursive_wrapper& operator=(const recursive_wrapper& rhs)
		{
		assign(rhs.get());
		return *this;
		}

	recursive_wrapper& operator=(recursive_wrapper&& rhs)
		{ item = std::move(rhs.item); }

	recursive_wrapper& operator=(const T& rhs)
		{
		assign(rhs);
		return *this;
		}

	recursive_wrapper& operator=(T&& rhs) noexcept
		{
		assign(std::move(rhs));
		return *this;
		}

	T& get()
		{ return *item; }

	const T& get() const
		{ return *item; }

private:

	template <typename U>
	void assign(U&& u)
		{ *item = std::forward<U>(u); }

	friend bool operator==(const recursive_wrapper& x,
	                       const recursive_wrapper& y)
		{ return *x.item == *y.item; }

	friend bool operator<(const recursive_wrapper& x,
	                      const recursive_wrapper& y)
		{ return *x.item < *y.item; }

	std::unique_ptr<T> item;
};

} // namespace detail

/**
 * A variant class (tagged/discriminated union).
 * @tparam Tag the type of the discriminator.  If this type is an enum value,
 *             it must start at 0 and increment sequentially by 1.
 * @tparam Ts the types the variant should assume.
 */
template <typename Tag, typename... Ts>
class variant : totally_ordered<variant<Tag, Ts...>> {

	// Workaround for http://stackoverflow.com/q/24433658/1170277
	template <typename T, typename...>
	struct front_type { using type = T; };

public:

	/**
	 * The type of the variant discriminator.
	 */
	using tag = Tag;

	/**
	 * The first type of the variant which is used for default-construction.
	 */
	using front = typename front_type<Ts...>::type;

	/**
	 * Construct a variant from a type tag.
	 * @param t the tag corresponding to the type assumed by returned variant.
	 * @return a variant with type corresponding to the tag argument.
	 */
	static variant make(tag t)
		{ return {factory{}, t}; }

	/**
	 * @return the tag of the active variant type.
	 */
	tag which() const
		{ return active; }

	/**
	 * Default-construct a variant with the first type.
	 */
	variant() noexcept
		{
		construct(front{});
		active = tag{};
		}

	/**
	  * Destruct variant by invoking destructor of the active instance.
	  */
	~variant() noexcept
		{ destruct(); }

	/**
	 * Constructs a variant from one of the discriminated types.
	 *
	 * @param x the value to construct the variant with.  Note that *x* must be
	 *          unambiguously convertible to one of the variant types.
	 */
	template <typename T, typename = disable_if_same_or_derived_t<variant, T>>
	variant(T&& x)
		{
		// A compile error here means that T is not unambiguously convertible to
		// any of the variant types.
		initializer<0, Ts...>::initialize(*this, std::forward<T>(x));
		}

	/**
	 * Construct a variant by copying another.
	 * @param other a variant to copy.
	 */
	variant(const variant& other)
		{
		other.apply_visitor_internal(copy_ctor(*this));
		active = other.active;
		}

	/**
	 * Construct a variant by stealing another.
	 * @param other a variant to steal.
	 */
	variant(variant&& other) noexcept
		{
		other.apply_visitor_internal(move_ctor(*this));
		active = other.active;
		}

	/**
	 * Assign another variant to this one by copying it.
	 * @param rhs a variant to copy
	 * @return reference of the assigned-to variant.
	 */
	variant& operator=(const variant& rhs)
		{
		rhs.apply_visitor_internal(assigner(*this, rhs.active));
		active = rhs.active;
		return *this;
		}

	/**
	 * Assign another variant to this one by stealing it.
	 * @param rhs a variant to copy
	 * @return reference of the assigned-to variant.
	 */
	variant& operator=(variant&& rhs) noexcept
		{
		rhs.apply_visitor_internal(move_assigner(*this, rhs.active));
		active = rhs.active;
		return *this;
		}

	template <typename Internal, typename Visitor, typename... Args>
	typename remove_reference_t<Visitor>::result_type
	apply(Visitor&& visitor, Args&&... args)
		{
		return visit_impl(active, Internal{}, storage,
		                  std::forward<Visitor>(visitor),
		                  std::forward<Args>(args)...);
		}

	template <typename Internal, typename Visitor, typename... Args>
	typename remove_reference_t<Visitor>::result_type
	apply(Visitor&& visitor, Args&&... args) const
		{
		return visit_impl(active, Internal{}, storage,
		                  std::forward<Visitor>(visitor),
		                  std::forward<Args>(args)...);
		}

private:

	aligned_union_t<0, Ts...> storage;
	tag active;

	struct default_ctor {
		using result_type = void;

		default_ctor(variant& arg_self) : self(arg_self) {}

		template <typename T>
		void operator()(const T&) const
			{ self.construct(T()); }

		variant& self;
	};

	struct copy_ctor {
		using result_type = void;

		copy_ctor(variant& arg_self) : self(arg_self) {}

		template <typename T>
		void operator()(const T& other) const
			{ self.construct(other); }

		variant& self;
	};

	struct move_ctor {
		using result_type = void;

		move_ctor(variant& arg_self) : self(arg_self) {}

		template <typename T>
		void operator()(T& rhs) const noexcept
			{
			static_assert(std::is_nothrow_move_constructible<T>{},
			              "T must not throw in move constructor");
			self.construct(std::move(rhs));
			}

		variant& self;
	};

	struct assigner {
		using result_type = void;

		assigner(variant& arg_self, tag arg_rhs_active)
			: self(arg_self), rhs_active(arg_rhs_active) {}

		template <typename Rhs>
		void operator()(const Rhs& rhs) const
			{
			static_assert(std::is_nothrow_destructible<Rhs>{},
			              "T must not throw in destructor");
			static_assert(std::is_nothrow_move_constructible<Rhs>{},
			              "T must not throw in move constructor");

			if ( self.active == rhs_active )
				*reinterpret_cast<Rhs*>(&self.storage) = rhs;
			else
				{
				Rhs tmp(rhs);
				self.destruct();
				self.construct(std::move(tmp));
				}
			}

		variant& self;
		tag rhs_active;
	};

	struct move_assigner {
		using result_type = void;

		move_assigner(variant& arg_self, tag arg_rhs_active)
			: self(arg_self), rhs_active(arg_rhs_active) {}

		template <typename Rhs>
		void operator()(Rhs& rhs) const noexcept
			{
			using rhs_type = typename std::remove_const<Rhs>::type;
			static_assert(std::is_nothrow_destructible<rhs_type>{},
			              "T must not throw in destructor");
			static_assert(std::is_nothrow_move_assignable<rhs_type>{},
			              "T must not throw in move assignment");
			static_assert(std::is_nothrow_move_constructible<rhs_type>{},
			              "T must not throw in move constructor");

			if ( self.active == rhs_active )
				*reinterpret_cast<rhs_type*>(&self.storage) = std::move(rhs);
			else
				{
				self.destruct();
				self.construct(std::move(rhs));
				}
			}

		variant& self;
		tag rhs_active;
	};

	struct dtor {
		using result_type = void;

		template <typename T>
		void operator()(T& x) const noexcept
			{
			static_assert(std::is_nothrow_destructible<T>{},
			              "T must not throw in destructor");
			x.~T();
			}
	};

	template <size_t TT, typename... Tail>
	struct initializer;

	template <size_t TT, typename T, typename... Tail>
	struct initializer<TT, T, Tail...> : public initializer<TT + 1, Tail...> {
		using base = initializer<TT + 1, Tail...>;
		using base::initialize;

		static void initialize(variant& v, T&& x)
			{
			v.construct(std::move(x));
			v.active = static_cast<tag>(TT);
			}

		static void initialize(variant& v, const T& x)
			{
			v.construct(x);
			v.active = static_cast<tag>(TT);
			}
	};

	template <size_t TT>
	struct initializer<TT> {
		void initialize(); // this should never match
	};

	template <typename T, typename Internal>
	static T& get_value(T& x, Internal)
		{ return x; }

	template <typename T, typename Internal>
	static const T& get_value(const T& x, Internal)
		{ return x; }

	template <typename T>
	static T& get_value(detail::recursive_wrapper<T>& x, std::false_type)
		{ return x.get(); }

	template <typename T>
	static const T& get_value(const detail::recursive_wrapper<T>& x,
	                          std::false_type)
		{ return x.get(); }

	template <typename T, typename Storage>
	using const_type = typename std::conditional<
	    std::is_const<remove_reference_t<Storage>>::value, T const, T>::type;

	template <typename T, typename Internal, typename Storage, typename Visitor,
	          typename... Args>
	static typename remove_reference_t<Visitor>::result_type
	invoke(Internal internal, Storage&& storage, Visitor&& visitor,
	       Args&&... args)
		{
		auto x = reinterpret_cast<const_type<T, Storage>*>(&storage);
		return visitor(get_value(*x, internal), args...);
		}

	template <typename Internal, typename Storage, typename Visitor,
	          typename... Args>
	static typename remove_reference_t<Visitor>::result_type
	visit_impl(tag which_active, Internal internal, Storage&& storage,
	           Visitor&& visitor, Args&&... args)
		{
		using result_type = typename remove_reference_t<Visitor>::result_type;
		using fn = result_type (*)(Internal, Storage&&, Visitor&&, Args&&...);
		static constexpr fn callers[sizeof...(Ts)] =
		{ &invoke<Ts, Internal, Storage, Visitor, Args...>... };

		assert(static_cast<size_t>(which_active) >= 0 &&
		       static_cast<size_t>(which_active) < sizeof...(Ts));

		return (*callers[static_cast<size_t>(which_active)])(internal,
		            std::forward<Storage>(storage),
		            std::forward<Visitor>(visitor),
		            std::forward<Args>(args)...);
		}

	struct factory {};

	variant(factory, tag t)
		: active(t)
		{ apply<std::false_type>(default_ctor(*this)); }

	template <typename Visitor>
	typename Visitor::result_type
	apply_visitor_internal(Visitor&& v)
		{ return apply<std::true_type>(std::forward<Visitor>(v)); }

	template <typename Visitor>
	typename Visitor::result_type
	apply_visitor_internal(Visitor&& v) const
		{ return apply<std::true_type>(std::forward<Visitor>(v)); }

	template <typename T>
	void construct(T&& x) noexcept(std::is_rvalue_reference<decltype(x)>{})
		{
		using type = typename std::remove_reference<T>::type;
		// FIXME: Somehow the compiler doesn't generate nothrow move ctors
		// for some of our custom types, even though they are annotated as such.
		// Needs investigation.
		//static_assert(std::is_nothrow_move_constructible<type>{},
		//              "move constructor of T must not throw");
		new (&storage) type(std::forward<T>(x));
		}

	void destruct() noexcept
		{ apply_visitor_internal(dtor()); }

	struct equals {
		using result_type = bool;

		equals(const variant& arg_self) : self(arg_self) {}

		template <typename Rhs>
		bool operator()(const Rhs& rhs) const
			{ return *reinterpret_cast<const Rhs*>(&self.storage) == rhs; }

		const variant& self;
	};

	struct less_than {
		using result_type = bool;

		less_than(const variant& arg_self) : self(arg_self) {}

		template <typename Rhs>
		bool operator()(const Rhs& rhs) const
			{ return *reinterpret_cast<const Rhs*>(&self.storage) < rhs; }

		const variant& self;
	};

	friend bool operator==(const variant& x, const variant& y)
		{ return x.active == y.active && y.apply_visitor_internal(equals{x}); }

	friend bool operator<(const variant& x, const variant& y)
		{
		if ( x.active == y.active )
			return y.apply_visitor_internal(less_than{x});
		else
			return x.active < y.active;
		}
};

/**
 * A variant with a defaulted tag type.
 */
template <typename... Ts>
using default_variant = variant<size_t, Ts...>;

/**
 * @tparam Visitor a class that provides operator()(T) for each type
 * "T" in the variant that it will be applied to and provides
 * Visitor::result_type type, which shall be the return type of each overload.
 * @param visitor object with visitor methods for each variant type.
 * @return a visitor that when invoked, will apply the visitor argument for
 * the active member of a variant.  e.g. May be used with std::for_each.
 */
template <typename Visitor>
detail::delayed_visitor<Visitor>
apply_visitor(Visitor&& visitor)
	{ return detail::delayed_visitor<Visitor>(std::move(visitor)); }

/**
 * @tparam Visitor a class that provides operator()(T) for each type
 * "T" in the variant that it will be applied to and provides
 * Visitor::result_type type, which shall be the return type of each overload.
 * @param visitor object with visitor methods for each variant type.
 * @return a visitor that when invoked, will apply the visitor argument for
 * the active member of a variant.  e.g. May be used with std::for_each.
 */
template <typename Visitor>
detail::delayed_visitor_wrapper<Visitor>
apply_visitor(Visitor& visitor)
	{ return detail::delayed_visitor_wrapper<Visitor>(visitor); }

/**
 * Applies an operation to the active member of a variant.
 * @tparam Visitor a class that provides operator()(T) for each type
 * "T" in the variant that it will be applied to and provides
 * Visitor::result_type type, which shall be the return type of each overload.
 * @tparam Visitable a variant type.
 * @param visitor object with visitor methods for each variant type.
 * @param visitable a variant object.
 * @return the result of applying the visitor to the active variant member.
 */
template <typename Visitor, typename Visitable>
typename remove_reference_t<Visitor>::result_type
apply_visitor(Visitor&& visitor, Visitable&& visitable)
	{
	return visitable.template
	       apply<std::false_type>(std::forward<Visitor>(visitor));
	}

/**
 * Applies an n-ary operation to active members of two variants.
 * @tparam Visitor a class that provides operator()(T) for each type
 * "T" in the variant that it will be applied to and provides
 * Visitor::result_type type, which shall be the return type of each overload.
 * @tparam V a variant type.
 * @tparam Vs a variant type.
 * @param visitor object with visitor methods for each variant type.
 * @param v a variant object.
 * @param vs a variant object.
 * @return the result of applying the n-ary visitor between the active members
 * of the variant object arguments.
 */
template <typename Visitor, typename V, typename... Vs>
typename remove_reference_t<Visitor>::result_type
apply_visitor(Visitor&& visitor, V&& v, Vs&&... vs)
	{
	return apply_visitor(detail::binary_visitor<Visitor, V>(visitor, v), vs...);
	}

/**
 * Allows variants to conform to the variant concept.  Other data types that
 * provide an extended interface around a variant member may provide overloads
 * of the const and non-const version of this expose function that return the
 * variant member in order to enable @see broker::util::visit(),
 * @see broker::util::get(), @see broker::util::is(), and
 * @see broker::util::which().
 */
template <typename Tag, typename... Ts>
variant<Tag, Ts...>& expose(variant<Tag, Ts...>& v)
	{ return v; }

/**
 * const version of @see broker::util::expose().
 */
template <typename Tag, typename... Ts>
const variant<Tag, Ts...>& expose(const variant<Tag, Ts...>& v)
	{ return v; }

/**
 * Applies an operation to active member of a variant by calling approprate
 * version of apply_visitor().
 * @return the result of applying the visitation operation to the active member
 * of the variant object argument(s).
 */
template <typename Visitor, typename... Vs>
typename remove_reference_t<Visitor>::result_type
visit(Visitor&& v, Vs&&... vs)
	{ return apply_visitor(std::forward<Visitor>(v), expose(vs)...); }

/**
 * @tparam T a type within the variant to retrieve.
 * @param v a variant from which to retrieve an active member.
 * @return a pointer to the variant member of type T if it is the active type
 * of the variant, else a null pointer.
 */
template <typename T, typename Visitable>
T* get(Visitable& v)
	{ return apply_visitor(detail::getter<T>{}, expose(v)); }

/**
 * const version of @see broker::util::get()
 */
template <typename T, typename Visitable>
const T* get(const Visitable& v)
	{ return apply_visitor(detail::getter<const T>{}, expose(v)); }

/**
 * @tparam T a type within the variant to check for active status.
 * @param v a variant to query for its active type.
 * @return whether the variant member of type T is the currently active member
 * of the variant.
 */
template <typename T, typename Visitable>
bool is(const Visitable& v)
	{ return get<T>(v) != nullptr; }

/**
 * @param v a variant to query for its active tag.
 * @return the tag of the currently active member of the variant.
 */
template <typename V>
typename V::tag which(const V& v)
	{ return expose(v).which(); }

} // namespace util

// Use these via ADL in broker namespace.
using util::visit;
using util::get;
using util::is;
using util::which;

} // namespace broker

namespace std {
template <typename Tag, typename... Ts>
struct hash<broker::util::variant<Tag, Ts...>> {
	using result_type = broker::util::detail::hasher::result_type;
	using argument_type = broker::util::variant<Tag, Ts...>;

	inline result_type operator()(const argument_type& v) const
		{ return broker::util::visit(broker::util::detail::hasher{}, v); }
};
}

#endif // BROKER_UTIL_VARIANT_HH
