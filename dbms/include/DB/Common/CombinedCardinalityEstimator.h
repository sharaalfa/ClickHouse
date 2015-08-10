#pragma once

#include <DB/Common/HashTable/SmallTable.h>
#include <DB/Common/HashTable/HashSet.h>
#include <statdaemons/HyperLogLogCounter.h>
#include <DB/Core/Defines.h>


namespace DB
{

namespace details
{

enum class ContainerType : UInt8 { SMALL = 1, MEDIUM = 2, LARGE = 3 };

static inline ContainerType max(const ContainerType & lhs, const ContainerType & rhs)
{
	UInt8 res = std::max(static_cast<UInt8>(lhs), static_cast<UInt8>(rhs));
	return static_cast<ContainerType>(res);
}

}

/** Для маленького количества ключей - массив фиксированного размера "на стеке".
  * Для среднего - выделяется HashSet.
  * Для большого - выделяется HyperLogLog.
  */
template
<
	typename Key,
	typename HashContainer,
	UInt8 small_set_size_max,
	UInt8 medium_set_power2_max,
	UInt8 K,
	typename Hash = IntHash32<Key>,
	typename DenominatorType = float
>
class CombinedCardinalityEstimator
{
public:
	using Self = CombinedCardinalityEstimator<Key, HashContainer, small_set_size_max, medium_set_power2_max, K, Hash, DenominatorType>;

private:
	using Small = SmallSet<Key, small_set_size_max>;
	using Medium = HashContainer;
	using Large = HyperLogLogCounter<K, Hash, DenominatorType>;

public:
	CombinedCardinalityEstimator()
	{
		setContainerType(details::ContainerType::SMALL);
	}

	~CombinedCardinalityEstimator()
	{
		destroy();
	}

	void insert(Key value)
	{
		auto container_type = getContainerType();

		if (container_type == details::ContainerType::SMALL)
		{
			if (small.find(value) == small.end())
			{
				if (!small.full())
					small.insert(value);
				else
				{
					toMedium();
					getContainer<Medium>().insert(value);
				}
			}
		}
		else if (container_type == details::ContainerType::MEDIUM)
		{
			auto & container = getContainer<Medium>();
			if (container.size() < medium_set_size_max)
				container.insert(value);
			else
			{
				toLarge();
				getContainer<Large>().insert(value);
			}
		}
		else if (container_type == details::ContainerType::LARGE)
			getContainer<Large>().insert(value);
	}

	UInt32 size() const
	{
		auto container_type = getContainerType();

		if (container_type == details::ContainerType::SMALL)
			return small.size();
		else if (container_type == details::ContainerType::MEDIUM)
			return getContainer<Medium>().size();
		else if (container_type == details::ContainerType::LARGE)
			return getContainer<Large>().size();
		else
			throw Poco::Exception("Internal error", ErrorCodes::LOGICAL_ERROR);
	}

	void merge(const Self & rhs)
	{
		auto container_type = getContainerType();
		auto max_container_type = details::max(container_type, rhs.getContainerType());

		if (container_type != max_container_type)
		{
			if (max_container_type == details::ContainerType::MEDIUM)
				toMedium();
			else if (max_container_type == details::ContainerType::LARGE)
				toLarge();
		}

		if (rhs.getContainerType() == details::ContainerType::SMALL)
		{
			for (const auto & x : rhs.small)
				insert(x);
		}
		else if (rhs.getContainerType() == details::ContainerType::MEDIUM)
		{
			for (const auto & x : rhs.getContainer<Medium>())
				insert(x);
		}
		else if (rhs.getContainerType() == details::ContainerType::LARGE)
			getContainer<Large>().merge(rhs.getContainer<Large>());
	}

	/// Можно вызывать только для пустого объекта.
	void read(DB::ReadBuffer & in)
	{
		UInt8 v;
		readBinary(v, in);
		auto container_type = static_cast<details::ContainerType>(v);

		if (container_type == details::ContainerType::SMALL)
			small.read(in);
		else if (container_type == details::ContainerType::MEDIUM)
		{
			toMedium();
			getContainer<Medium>().read(in);
		}
		else if (container_type == details::ContainerType::LARGE)
		{
			toLarge();
			getContainer<Large>().read(in);
		}
	}

	void readAndMerge(DB::ReadBuffer & in)
	{
		auto container_type = getContainerType();

		UInt8 v;
		readBinary(v, in);
		auto rhs_container_type = static_cast<details::ContainerType>(v);

		auto max_container_type = details::max(container_type, rhs_container_type);

		if (container_type != max_container_type)
		{
			if (max_container_type == details::ContainerType::MEDIUM)
				toMedium();
			else if (max_container_type == details::ContainerType::LARGE)
				toLarge();
		}

		if (rhs_container_type == details::ContainerType::SMALL)
		{
			typename Small::Reader reader(in);
			while (reader.next())
				insert(reader.get());
		}
		else if (rhs_container_type == details::ContainerType::MEDIUM)
		{
			typename Medium::Reader reader(in);
			while (reader.next())
				insert(reader.get());
		}
		else if (rhs_container_type == details::ContainerType::LARGE)
			getContainer<Large>().readAndMerge(in);
	}

	void write(DB::WriteBuffer & out) const
	{
		auto container_type = getContainerType();
		writeBinary(static_cast<UInt8>(container_type), out);

		if (container_type == details::ContainerType::SMALL)
			small.write(out);
		else if (container_type == details::ContainerType::MEDIUM)
			getContainer<Medium>().write(out);
		else if (container_type == details::ContainerType::LARGE)
			getContainer<Large>().write(out);
	}

private:
	void toMedium()
	{
		if (getContainerType() != details::ContainerType::SMALL)
			throw Poco::Exception("Internal error", ErrorCodes::LOGICAL_ERROR);

		auto tmp_medium = std::make_unique<Medium>();

		for (const auto & x : small)
			tmp_medium->insert(x);

		new (&medium) std::unique_ptr<Medium>{ std::move(tmp_medium) };

		setContainerType(details::ContainerType::MEDIUM);

		if (current_memory_tracker)
			current_memory_tracker->alloc(sizeof(medium));
	}

	void toLarge()
	{
		auto container_type = getContainerType();

		if ((container_type != details::ContainerType::SMALL) && (container_type != details::ContainerType::MEDIUM))
			throw Poco::Exception("Internal error", ErrorCodes::LOGICAL_ERROR);

		auto tmp_large = std::make_unique<Large>();

		if (container_type == details::ContainerType::SMALL)
		{
			for (const auto & x : small)
				tmp_large->insert(x);
		}
		else if (container_type == details::ContainerType::MEDIUM)
		{
			for (const auto & x : getContainer<Medium>())
				tmp_large->insert(x);

			destroy();
		}

		new (&large) std::unique_ptr<Large>{ std::move(tmp_large) };

		setContainerType(details::ContainerType::LARGE);

		if (current_memory_tracker)
			current_memory_tracker->alloc(sizeof(large));

	}

	void NO_INLINE destroy()
	{
		auto container_type = getContainerType();

		clearContainerType();

		if (container_type == details::ContainerType::MEDIUM)
		{
			medium.std::unique_ptr<Medium>::~unique_ptr();
			if (current_memory_tracker)
				current_memory_tracker->free(sizeof(medium));
		}
		else if (container_type == details::ContainerType::LARGE)
		{
			large.std::unique_ptr<Large>::~unique_ptr();
			if (current_memory_tracker)
				current_memory_tracker->free(sizeof(large));
		}
	}

	template<typename T>
	inline T & getContainer()
	{
		return *reinterpret_cast<T *>(address & mask);
	}

	template<typename T>
	inline const T & getContainer() const
	{
		return *reinterpret_cast<T *>(address & mask);
	}

	void setContainerType(details::ContainerType t)
	{
		address |= static_cast<UInt8>(t);
	}

	inline details::ContainerType getContainerType() const
	{
		return static_cast<details::ContainerType>(address & ~mask);
	}

	void clearContainerType()
	{
		address &= mask;
	}

private:
	Small small;
	union
	{
		std::unique_ptr<Medium> medium;
		std::unique_ptr<Large> large;
		UInt64 address = 0;
	};
	static const UInt64 mask = 0xFFFFFFFC;
	static const UInt32 medium_set_size_max = 1UL << medium_set_power2_max;
};

}
