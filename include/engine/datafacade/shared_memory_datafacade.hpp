#ifndef SHARED_MEMORY_DATAFACADE_HPP
#define SHARED_MEMORY_DATAFACADE_HPP

// implements all data storage when shared memory _IS_ used

#include "storage/shared_barriers.hpp"
#include "storage/shared_datatype.hpp"
#include "storage/shared_memory.hpp"
#include "engine/datafacade/memory_datafacade_base.hpp"

namespace osrm
{
namespace engine
{
namespace datafacade
{

class SharedDataFacade : public MemoryDataFacadeBase
{

  protected:
    std::shared_ptr<storage::SharedBarriers> shared_barriers;
    storage::SharedDataType layout_region;
    storage::SharedDataType data_region;
    unsigned shared_timestamp;

    SharedDataFacade() {}

  public:
    // this function handle the deallocation of the shared memory it we can prove it will not be
    // used anymore
    virtual ~SharedDataFacade()
    {
        boost::interprocess::scoped_lock<boost::interprocess::named_sharable_mutex> exclusive_lock(
            data_region == storage::DATA_1 ? shared_barriers->regions_1_mutex
                                           : shared_barriers->regions_2_mutex,
            boost::interprocess::defer_lock);

        // if this returns false this is still in use
        if (exclusive_lock.try_lock())
        {
            // Now check if this is still the newest dataset
            const boost::interprocess::sharable_lock<boost::interprocess::named_upgradable_mutex>
                lock(shared_barriers->current_regions_mutex);

            auto shared_regions = storage::makeSharedMemory(storage::CURRENT_REGIONS);
            const auto current_timestamp =
                static_cast<const storage::SharedDataTimestamp *>(shared_regions->Ptr());

            if (current_timestamp->timestamp == shared_timestamp)
            {
                util::SimpleLogger().Write(logDEBUG) << "Retaining data with shared timestamp "
                                                     << shared_timestamp;
            }
            else
            {
                storage::SharedMemory::Remove(data_region);
                storage::SharedMemory::Remove(layout_region);
            }
        }
    }

    SharedDataFacade(const std::shared_ptr<storage::SharedBarriers> &shared_barriers_,
                     storage::SharedDataType layout_region_,
                     storage::SharedDataType data_region_,
                     unsigned shared_timestamp_)
        : shared_barriers(shared_barriers_), layout_region(layout_region_),
          data_region(data_region_), shared_timestamp(shared_timestamp_)
    {
        util::SimpleLogger().Write(logDEBUG) << "Loading new data with shared timestamp "
                                             << shared_timestamp;

        BOOST_ASSERT(storage::SharedMemory::RegionExists(layout_region));
        m_layout_memory = storage::makeSharedMemory(layout_region);

        data_layout = static_cast<storage::DataLayout *>(m_layout_memory->Ptr());

        BOOST_ASSERT(storage::SharedMemory::RegionExists(data_region));
        m_large_memory = storage::makeSharedMemory(data_region);
        memory_block = (char *)(m_large_memory->Ptr());
        Init();
    }
};
}
}
}

#endif // SHARED_MEMORY_DATAFACADE_HPP
