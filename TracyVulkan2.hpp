#ifndef __TRACYVULKAN2_HPP__
#define __TRACYVULKAN2_HPP__

#include "TracyVulkanFwd.hpp"

#if !defined TRACY_VULKAN_ENABLE

#define TracyVkContext(x,y,z,w) nullptr
#define TracyVkContextCalibrated(x,y,z,w,a,b) nullptr
#define TracyVkDestroy(x)
#define TracyVkNamedZone(c,x,y,z,w)
#define TracyVkNamedZoneC(c,x,y,z,w,a)
#define TracyVkZone(c,x,y)
#define TracyVkZoneC(c,x,y,z)
#define TracyVkCollect(c,x)

#define TracyVkNamedZoneS(c,x,y,z,w,a)
#define TracyVkNamedZoneCS(c,x,y,z,w,v,a)
#define TracyVkZoneS(c,x,y,z)
#define TracyVkZoneCS(c,x,y,z,w)

namespace tracy
{
class VkCtxScope {};
}

using TracyVkCtx = void*;

#else

#include <assert.h>
#include <stdlib.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include "Tracy.hpp"
#include "client/TracyProfiler.hpp"
#include "client/TracyCallstack.hpp"

namespace tracy
{

class VkCtx
{
    friend class VkCtxScope;

    static constexpr std::uint64_t MaxQueries = 4*1024;

public:
    VkCtx( VkPhysicalDevice physdev, VkDevice device, VkQueue queue, VkCommandBuffer cmdbuf, PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT _vkGetPhysicalDeviceCalibrateableTimeDomainsEXT, PFN_vkGetCalibratedTimestampsEXT _vkGetCalibratedTimestampsEXT )
        : m_device( device )
        , m_timeDomain( VK_TIME_DOMAIN_DEVICE_EXT )
        , m_context( GetGpuCtxCounter().fetch_add( 1, std::memory_order_relaxed ) )
        , m_queryCount( MaxQueries )
        , m_vkGetCalibratedTimestampsEXT( _vkGetCalibratedTimestampsEXT )
    {
        assert( m_context != 255 );

        if( _vkGetPhysicalDeviceCalibrateableTimeDomainsEXT && _vkGetCalibratedTimestampsEXT )
        {
            uint32_t num;
            _vkGetPhysicalDeviceCalibrateableTimeDomainsEXT( physdev, &num, nullptr );
            if( num > 4 ) num = 4;
            VkTimeDomainEXT data[4];
            _vkGetPhysicalDeviceCalibrateableTimeDomainsEXT( physdev, &num, data );
            for( uint32_t i=0; i<num; i++ )
            {
                // TODO VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT
                if( data[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT )
                {
                    m_timeDomain = data[i];
                    break;
                }
            }
        }

        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties( physdev, &prop );
        const float period = prop.limits.timestampPeriod;

        VkQueryPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryCount = m_queryCount;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        while( vkCreateQueryPool( device, &poolInfo, nullptr, &m_queryPool ) != VK_SUCCESS )
        {
            m_queryCount /= 2;
            poolInfo.queryCount = m_queryCount;
        }

        for (std::uint32_t i = m_queryCount; i > 0; --i) {
            m_availableQueries.push_back(i-1);
        }

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdbuf;

        vkBeginCommandBuffer( cmdbuf, &beginInfo );
        vkCmdResetQueryPool( cmdbuf, m_queryPool, 0, m_queryCount-1 );
        vkEndCommandBuffer( cmdbuf );
        vkQueueSubmit( queue, 1, &submitInfo, VK_NULL_HANDLE );
        vkQueueWaitIdle( queue );

        int64_t tcpu, tgpu;
        if( m_timeDomain == VK_TIME_DOMAIN_DEVICE_EXT )
        {
            vkBeginCommandBuffer( cmdbuf, &beginInfo );
            vkCmdWriteTimestamp( cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0 );
            vkEndCommandBuffer( cmdbuf );
            vkQueueSubmit( queue, 1, &submitInfo, VK_NULL_HANDLE );
            vkQueueWaitIdle( queue );

            tcpu = Profiler::GetTime();
            vkGetQueryPoolResults( device, m_queryPool, 0, 1, sizeof( tgpu ), &tgpu, sizeof( tgpu ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );

            vkBeginCommandBuffer( cmdbuf, &beginInfo );
            vkCmdResetQueryPool( cmdbuf, m_queryPool, 0, 1 );
            vkEndCommandBuffer( cmdbuf );
            vkQueueSubmit( queue, 1, &submitInfo, VK_NULL_HANDLE );
            vkQueueWaitIdle( queue );
        }
        else
        {
            enum { NumProbes = 32 };

            VkCalibratedTimestampInfoEXT spec[2] = {
                { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT },
                { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, m_timeDomain },
            };
            uint64_t ts[2];
            uint64_t deviation[NumProbes];
            for( int i=0; i<NumProbes; i++ )
            {
                _vkGetCalibratedTimestampsEXT( device, 2, spec, ts, deviation+i );
            }
            uint64_t minDeviation = deviation[0];
            for( int i=1; i<NumProbes; i++ )
            {
                if( minDeviation > deviation[i] )
                {
                    minDeviation = deviation[i];
                }
            }
            m_deviation = minDeviation * 3 / 2;

            m_qpcToNs = int64_t( 1000000000. / GetFrequencyQpc() );

            Calibrate( device, m_prevCalibration, tgpu );
            tcpu = Profiler::GetTime();
        }

        uint8_t flags = 0;
        if( m_timeDomain != VK_TIME_DOMAIN_DEVICE_EXT ) flags |= GpuContextCalibration;

        TracyLfqPrepare( QueueType::GpuNewContext );
        MemWrite( &item->gpuNewContext.cpuTime, tcpu );
        MemWrite( &item->gpuNewContext.gpuTime, tgpu );
        MemWrite<std::uint64_t>( &item->gpuNewContext.thread, 0);
        MemWrite( &item->gpuNewContext.period, period );
        MemWrite( &item->gpuNewContext.context, m_context );
        MemWrite( &item->gpuNewContext.flags, flags );
        MemWrite( &item->gpuNewContext.type, GpuContextType::Vulkan );
        TracyLfqCommit;

#ifdef TRACY_ON_DEMAND
        GetProfiler().DeferItem( *item );
#endif


        m_queryResults.resize(m_queryCount);
    }

    ~VkCtx()
    {
        vkDestroyQueryPool( m_device, m_queryPool, nullptr );
    }

    void Collect( VkCommandBuffer cmdbuf )
    {
        ZoneScopedC( Color::Red4 );

#ifdef TRACY_ON_DEMAND
        if( !GetProfiler().IsConnected() )
        {
            vkCmdResetQueryPool( cmdbuf, m_queryPool, 0, m_queryCount-1 );
            int64_t tgpu;
            if( m_timeDomain != VK_TIME_DOMAIN_DEVICE_EXT ) Calibrate( m_device, m_prevCalibration, tgpu );
            return;
        }
#endif

        vkGetQueryPoolResults(
            m_device,
            m_queryPool,
            0,
            m_queryCount-1,
            sizeof( int64_t ) * m_queryCount,
            m_queryResults.data(),
            sizeof( int64_t ),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );

        static std::vector<Context*> contexts;
        for( unsigned int idx=0; idx< m_queryCount; idx++ )
        {
            if (m_queryResults[idx] != 0) {
                if (auto ctxItr = m_queryContexts.find(idx); ctxItr == m_queryContexts.end()) {
                    continue;
                }
                auto &ctx = m_queryContexts[idx];
                if (ctx->endQueryId == (uint16_t)idx) {
                    contexts.push_back(ctx.get());
                }
            }
        }

        std::sort(contexts.begin(), contexts.end(), [this](const auto &a, const auto &b) {
            return m_queryResults[a->startQueryId] < m_queryResults[b->startQueryId];
        });

        for (const auto& ctx : contexts) {
            {
                TracyLfqPrepare( QueueType::GpuZoneBegin )
                MemWrite( &item->gpuZoneBegin.cpuTime, ctx->cpuStartTime );
                MemWrite( &item->gpuZoneBegin.srcloc, ctx->srcLoc );
                MemWrite( &item->gpuZoneBegin.thread, ctx->thread );
                MemWrite( &item->gpuZoneBegin.queryId, ctx->startQueryId );
                MemWrite( &item->gpuZoneBegin.context, m_context );
                TracyLfqCommit;
            }
            {
                TracyLfqPrepare( QueueType::GpuZoneEnd )
                MemWrite( &item->gpuZoneEnd.cpuTime, ctx->cpuEndTime );
                MemWrite( &item->gpuZoneEnd.thread, ctx->thread );
                MemWrite( &item->gpuZoneEnd.queryId, ctx->endQueryId );
                MemWrite( &item->gpuZoneEnd.context, m_context );
                TracyLfqCommit;
            }
            {
                TracyLfqPrepare(QueueType::GpuTime);
                MemWrite( &item->gpuTime.gpuTime, m_queryResults[ctx->startQueryId] );
                MemWrite( &item->gpuTime.queryId, ctx->startQueryId );
                MemWrite( &item->gpuTime.context, m_context );
                TracyLfqCommit;
            }
            {
                TracyLfqPrepare(QueueType::GpuTime);
                MemWrite( &item->gpuTime.gpuTime, m_queryResults[ctx->endQueryId] );
                MemWrite( &item->gpuTime.queryId, ctx->endQueryId );
                MemWrite( &item->gpuTime.context, m_context );
                TracyLfqCommit;
            }
        }

        contexts.clear();

        if( m_timeDomain != VK_TIME_DOMAIN_DEVICE_EXT )
        {
            int64_t tgpu, tcpu;
            Calibrate( m_device, tcpu, tgpu );
            const auto refCpu = Profiler::GetTime();
            const auto delta = tcpu - m_prevCalibration;
            if( delta > 0 )
            {
                m_prevCalibration = tcpu;
                TracyLfqPrepare(QueueType::GpuCalibration);
                MemWrite( &item->gpuCalibration.gpuTime, tgpu );
                MemWrite( &item->gpuCalibration.cpuTime, refCpu );
                MemWrite( &item->gpuCalibration.cpuDelta, delta );
                MemWrite( &item->gpuCalibration.context, m_context );
                TracyLfqCommit;
            }
        }

        vkCmdResetQueryPool( cmdbuf, m_queryPool, 0, m_queryCount-1 );
    }


    tracy_force_inline unsigned int FreeQueries(VkCommandBuffer cmdbuf)
    {
        for (const auto &v : m_usedQueries[cmdbuf]) {
            m_availableQueries.push_back(v);
        }
        m_usedQueries[cmdbuf].clear();
    }

private:
    tracy_force_inline unsigned int AllocQueryId(VkCommandBuffer cmdbuf)
    {
        assert( ! m_availableQueries.empty() );
        const auto id = m_availableQueries.back();
        m_availableQueries.pop_back();
        if (auto result = m_usedQueries.find(cmdbuf); result != m_usedQueries.end()) {
            result->second.push_back(id);
        } else {
            m_usedQueries[cmdbuf] = {};
            m_usedQueries[cmdbuf].push_back(id);
        }
        return id;
    }

    tracy_force_inline uint8_t GetId() const
    {
        return m_context;
    }

    tracy_force_inline void Calibrate( VkDevice device, int64_t& tCpu, int64_t& tGpu )
    {
        assert( m_timeDomain != VK_TIME_DOMAIN_DEVICE_EXT );
        VkCalibratedTimestampInfoEXT spec[2] = {
            { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT },
            { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, m_timeDomain },
        };
        uint64_t ts[2];
        uint64_t deviation;
        do
        {
            m_vkGetCalibratedTimestampsEXT( device, 2, spec, ts, &deviation );
        }
        while( deviation > m_deviation );

#if defined _WIN32 || defined __CYGWIN__
        tGpu = ts[0];
        tCpu = ts[1] * m_qpcToNs;
#else
        assert( false );
#endif
    }

    VkDevice m_device;
    VkQueryPool m_queryPool;
    VkTimeDomainEXT m_timeDomain;
    uint64_t m_deviation;
    int64_t m_qpcToNs;
    int64_t m_prevCalibration;
    uint8_t m_context;

    unsigned int m_queryCount;
    std::vector<std::uint32_t> m_availableQueries;
    std::unordered_map<VkCommandBuffer, std::vector<std::uint32_t>> m_usedQueries;
    std::vector<std::int64_t> m_queryResults;
    struct Context {
        int64_t cpuStartTime = -1;
        int64_t cpuEndTime = -1;
        uint64_t srcLoc = 0;
        uint64_t thread = 0;
        uint16_t startQueryId = 65535;
        uint16_t endQueryId = 65535;
    };
    std::unordered_map<std::uint32_t, std::shared_ptr<Context>> m_queryContexts;

    PFN_vkGetCalibratedTimestampsEXT m_vkGetCalibratedTimestampsEXT;
};

class VkCtxScope
{
public:
    tracy_force_inline VkCtxScope( VkCtx* ctx, uint64_t srcloc, VkCommandBuffer cmdbuf, bool is_active )
#ifdef TRACY_ON_DEMAND
        : m_active( is_active && GetProfiler().IsConnected() )
#else
        : m_active( is_active )
#endif
        , m_cmdbuf(cmdbuf)
        , m_ctx(ctx)
    {
        if( !m_active ) return;

        const auto queryId = ctx->AllocQueryId(m_cmdbuf);
        m_ctx->m_queryContexts[queryId] = std::make_shared<VkCtx::Context>();
        m_queryContext = m_ctx->m_queryContexts[queryId];
        m_queryContext->cpuStartTime = Profiler::GetTime();
        m_queryContext->srcLoc = (uint64_t)srcloc;
        m_queryContext->thread = 0;
        m_queryContext->startQueryId = (uint16_t)queryId;

        vkCmdWriteTimestamp( m_cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->m_queryPool, queryId );
    }

    tracy_force_inline ~VkCtxScope()
    {
        if( !m_active ) return;

        const auto queryId = m_ctx->AllocQueryId(m_cmdbuf);
        m_ctx->m_queryContexts[queryId] = m_queryContext;
        m_queryContext->cpuEndTime = Profiler::GetTime();
        m_queryContext->endQueryId = (uint16_t) queryId;

        vkCmdWriteTimestamp( m_cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ctx->m_queryPool, queryId );
    }

private:
    const bool m_active;

    VkCommandBuffer m_cmdbuf;
    VkCtx* m_ctx;
    std::shared_ptr<VkCtx::Context> m_queryContext;
};

static inline VkCtx* CreateVkContext( VkPhysicalDevice physdev, VkDevice device, VkQueue queue, VkCommandBuffer cmdbuf, PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT gpdctd, PFN_vkGetCalibratedTimestampsEXT gct )
{
    InitRPMallocThread();
    auto ctx = (VkCtx*)tracy_malloc( sizeof( VkCtx ) );
    new(ctx) VkCtx( physdev, device, queue, cmdbuf, gpdctd, gct );
    return ctx;
}

static inline void DestroyVkContext( VkCtx* ctx )
{
    ctx->~VkCtx();
    tracy_free( ctx );
}

}

using TracyVkCtx = tracy::VkCtx*;

#define TracyVkContext( physdev, device, queue, cmdbuf ) tracy::CreateVkContext( physdev, device, queue, cmdbuf, nullptr, nullptr );
#define TracyVkContextCalibrated( physdev, device, queue, cmdbuf, gpdctd, gct ) tracy::CreateVkContext( physdev, device, queue, cmdbuf, gpdctd, gct );
#define TracyVkDestroy( ctx ) tracy::DestroyVkContext( ctx );
#if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
#  define TracyVkNamedZone( ctx, varname, cmdbuf, name, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::VkCtxScope varname( ctx, &TracyConcat(__tracy_gpu_source_location,__LINE__), cmdbuf, TRACY_CALLSTACK, active );
#  define TracyVkNamedZoneC( ctx, varname, cmdbuf, name, color, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::VkCtxScope varname( ctx, &TracyConcat(__tracy_gpu_source_location,__LINE__), cmdbuf, TRACY_CALLSTACK, active );
#  define TracyVkZone( ctx, cmdbuf, name ) TracyVkNamedZoneS( ctx, ___tracy_gpu_zone, cmdbuf, name, TRACY_CALLSTACK, true )
#  define TracyVkZoneL( ctx, cmdbuf, srcloc ) tracy::VkCtxScope varname( ctx, reinterpret_cast<tracy::SourceLocationData*>(srcloc), cmdbuf, TRACY_CALLSTACK, true )
#  define TracyVkZoneC( ctx, cmdbuf, name, color ) TracyVkNamedZoneCS( ctx, ___tracy_gpu_zone, cmdbuf, name, color, TRACY_CALLSTACK, true )
#else
#  define TracyVkNamedZone( ctx, varname, cmdbuf, name, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::VkCtxScope varname( ctx, reinterpret_cast<uint64_t>(&TracyConcat(__tracy_gpu_source_location,__LINE__)), cmdbuf, active );
#  define TracyVkNamedZoneC( ctx, varname, cmdbuf, name, color, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::VkCtxScope varname( ctx, &TracyConcat(__tracy_gpu_source_location,__LINE__), cmdbuf, active );
#  define TracyVkZone( ctx, cmdbuf, name ) TracyVkNamedZone( ctx, ___tracy_gpu_zone, cmdbuf, name, true )
#  define TracyVkZoneL( ctx, cmdbuf, srcloc ) tracy::VkCtxScope ___tracy_gpu_zone( ctx, srcloc, cmdbuf, true )
#  define TracyVkZoneC( ctx, cmdbuf, name, color ) TracyVkNamedZoneC( ctx, ___tracy_gpu_zone, cmdbuf, name, color, true )
#endif
#define TracyVkCollect( ctx, cmdbuf ) ctx->Collect( cmdbuf );

#ifdef TRACY_HAS_CALLSTACK
#  define TracyVkNamedZoneS( ctx, varname, cmdbuf, name, depth, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::VkCtxScope varname( ctx, &TracyConcat(__tracy_gpu_source_location,__LINE__), cmdbuf, depth, active );
#  define TracyVkNamedZoneCS( ctx, varname, cmdbuf, name, color, depth, active ) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::VkCtxScope varname( ctx, &TracyConcat(__tracy_gpu_source_location,__LINE__), cmdbuf, depth, active );
#  define TracyVkZoneS( ctx, cmdbuf, name, depth ) TracyVkNamedZoneS( ctx, ___tracy_gpu_zone, cmdbuf, name, depth, true )
#  define TracyVkZoneCS( ctx, cmdbuf, name, color, depth ) TracyVkNamedZoneCS( ctx, ___tracy_gpu_zone, cmdbuf, name, color, depth, true )
#else
#  define TracyVkNamedZoneS( ctx, varname, cmdbuf, name, depth, active ) TracyVkNamedZone( ctx, varname, cmdbuf, name, active )
#  define TracyVkNamedZoneCS( ctx, varname, cmdbuf, name, color, depth, active ) TracyVkNamedZoneC( ctx, varname, cmdbuf, name, color, active )
#  define TracyVkZoneS( ctx, cmdbuf, name, depth ) TracyVkZone( ctx, cmdbuf, name )
#  define TracyVkZoneCS( ctx, cmdbuf, name, color, depth ) TracyVkZoneC( ctx, cmdbuf, name, color )
#endif

#endif

#endif
