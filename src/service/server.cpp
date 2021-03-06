#include <cascade/cascade.hpp>
#include <cascade/service.hpp>
#include <cascade/service_types.hpp>
#include <cascade/object.hpp>
#include <sys/prctl.h>
#include <derecho/conf/conf.hpp>
#include <derecho/utils/logger.hpp>
#include <dlfcn.h>

#define PROC_NAME "cascade_server"

using namespace derecho::cascade;

#ifndef NDEBUG
inline void dump_layout(const json& layout) {
    int tid = 0;
    for (const auto& pertype:layout) {
        int sidx = 0;
        for (const auto& persubgroup:pertype ) {
            dbg_default_trace("subgroup={}.{},layout={}.",tid,sidx,persubgroup.dump());
            sidx ++;
        }
        tid ++;
    }
}
#endif//NDEBUG

int main(int argc, char** argv) {
    // set proc name
    if( prctl(PR_SET_NAME, PROC_NAME, 0, 0, 0) != 0 ) {
        dbg_default_warn("Cannot set proc name to {}.", PROC_NAME);
    }
    dbg_default_trace("set proc name to {}", PROC_NAME);
    // load configuration
    auto group_layout = json::parse(derecho::getConfString(CONF_GROUP_LAYOUT));
#ifndef NDEBUG
    dbg_default_trace("load layout:");
    dump_layout(group_layout);
#endif//NDEBUG
    // load on_data_library
    std::string ondata_library = "";
    if (derecho::hasCustomizedConfKey(CONF_ONDATA_LIBRARY)) {
        ondata_library = derecho::getConfString(CONF_ONDATA_LIBRARY);
    }

    std::shared_ptr<CriticalDataPathObserver<VolatileCascadeStoreWithStringKey>> cdpo_vcss_ptr;
    std::shared_ptr<CriticalDataPathObserver<PersistentCascadeStoreWithStringKey>> cdpo_pcss_ptr;
    std::shared_ptr<OffCriticalDataPathObserver> ocdpo_ptr;
    void (*on_cascade_initialization)() = nullptr;
    void (*on_cascade_exit)() = nullptr;
    std::shared_ptr<CriticalDataPathObserver<VolatileCascadeStoreWithStringKey>> (*get_cdpo_vcss)() = nullptr;
    std::shared_ptr<CriticalDataPathObserver<PersistentCascadeStoreWithStringKey>> (*get_cdpo_pcss)() = nullptr;
    std::shared_ptr<OffCriticalDataPathObserver> (*get_ocdpo)() = nullptr;
    void* dl_handle = nullptr;

    if (ondata_library.size()>0) {
        dl_handle = dlopen(ondata_library.c_str(),RTLD_LAZY);
        if (!dl_handle) {
            dbg_default_error("Failed to load shared ondata_library:{}. error={}", ondata_library, dlerror());
            return -1;
        }
        // TODO: find an dynamic/automatic way to get the mangled symbols.
        // 1 - on_cascade_initialization
        *reinterpret_cast<void **>(&on_cascade_initialization) = dlsym(dl_handle, "_ZN7derecho7cascade25on_cascade_initializationEv");
        if (on_cascade_initialization == nullptr) {
            dbg_default_error("Failed to load on_cascade_initialization(). error={}", dlerror());
            dlclose(dl_handle);
            return -1;
        }
        // 2 - on_cascade_exit
        *reinterpret_cast<void **>(&on_cascade_exit) = dlsym(dl_handle, "_ZN7derecho7cascade15on_cascade_exitEv");
        if (on_cascade_exit == nullptr) {
            dbg_default_error("Failed to load on_cascade_exit(). error={}", dlerror());
            dlclose(dl_handle);
            return -1;
        }
        // 3 - get cdpos
        *reinterpret_cast<void **>(&get_cdpo_vcss) = dlsym(dl_handle, "_ZN7derecho7cascade31get_critical_data_path_observerINS0_20VolatileCascadeStoreINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS0_19ObjectWithStringKeyEXadL_ZNS9_2IKB5cxx11EEEXadL_ZNS9_2IVEEEEEEESt10shared_ptrINS0_24CriticalDataPathObserverIT_EEEv");
        if (get_cdpo_vcss == nullptr) {
            dbg_default_warn("Failed to load get_cdpo_vcss(). error={}", dlerror());
        }
        *reinterpret_cast<void **>(&get_cdpo_pcss) = dlsym(dl_handle, "_ZN7derecho7cascade31get_critical_data_path_observerINS0_22PersistentCascadeStoreINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS0_19ObjectWithStringKeyEXadL_ZNS9_2IKB5cxx11EEEXadL_ZNS9_2IVEEELN10persistent11StorageTypeE0EEEEESt10shared_ptrINS0_24CriticalDataPathObserverIT_EEEv");
        if (get_cdpo_pcss == nullptr) {
            dbg_default_warn("Failed to load get_cdpo_pcss(). error={}", dlerror());
        }
        // 4 - get the off critical data path handler
        *reinterpret_cast<void **>(&get_ocdpo) = dlsym(dl_handle, "_ZN7derecho7cascade35get_off_critical_data_path_observerEv");
        if (get_ocdpo == nullptr) {
            dbg_default_error("Failed to load get_ocdpo(). error={}", dlerror());
            dlclose(dl_handle);
            return -1;
        }
    }

    // initialize
    if (on_cascade_initialization) {
        on_cascade_initialization();
    }

    // create service
    if (get_cdpo_vcss) {
        cdpo_vcss_ptr = std::move(get_cdpo_vcss());
    }
    if (get_cdpo_pcss) {
        cdpo_pcss_ptr = std::move(get_cdpo_pcss());
    }
    if (get_ocdpo) {
        ocdpo_ptr = std::move(get_ocdpo());
    }

    auto vcss_factory = [&cdpo_vcss_ptr](persistent::PersistentRegistry*, derecho::subgroup_id_t, ICascadeContext* context_ptr) {
        return std::make_unique<VolatileCascadeStoreWithStringKey>(cdpo_vcss_ptr.get(),context_ptr);
    };
    auto pcss_factory = [&cdpo_pcss_ptr](persistent::PersistentRegistry* pr, derecho::subgroup_id_t, ICascadeContext* context_ptr) {
        return std::make_unique<PersistentCascadeStoreWithStringKey>(pr,cdpo_pcss_ptr.get(),context_ptr);
    };
    dbg_default_trace("starting service...");
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::start(group_layout,ocdpo_ptr.get(),
            {cdpo_vcss_ptr.get(),cdpo_pcss_ptr.get()},
            vcss_factory,pcss_factory);
    dbg_default_trace("started service, waiting till it ends.");
    std::cout << "Press Enter to Shutdown." << std::endl;
    std::cin.get();
    // wait for service to quit.
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::shutdown(false);
    dbg_default_trace("shutdown service gracefully");
    // you can do something here to parallel the destructing process.
    Service<VolatileCascadeStoreWithStringKey,PersistentCascadeStoreWithStringKey>::wait();
    dbg_default_trace("Finish shutdown.");

    // exit
    if (on_cascade_exit) {
        on_cascade_exit();
    }
    if (dl_handle) {
        dlclose(dl_handle);
    }
    return 0;
}
