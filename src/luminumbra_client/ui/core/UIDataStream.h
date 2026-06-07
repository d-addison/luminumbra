#pragma once

#include "core/Log.h"
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <any>
#include <typeindex>

namespace Luminumbra::Client::UI {

// Forward declarations
class UIDataStream;
class DataBinding;

// Type-erased data binding interface
class IDataBinding {
public:
    virtual ~IDataBinding() = default;
    virtual void Update() = 0;
    virtual bool HasChanged() const = 0;
    virtual void ClearChanged() = 0;
    virtual std::type_index GetTypeIndex() const = 0;
    virtual const std::any& GetValue() const = 0;
    virtual void SetValue(const std::any& value) = 0;
    virtual std::string GetValueAsString() const = 0;
};

// Typed data binding implementation
template<typename T>
class TypedDataBinding : public IDataBinding {
public:
    using Getter = std::function<T()>;
    using Setter = std::function<void(const T&)>;
    using Validator = std::function<bool(const T&)>;
    using Formatter = std::function<std::string(const T&)>;
    
    TypedDataBinding(const std::string& id, Getter getter = nullptr, Setter setter = nullptr)
        : m_id(id), m_getter(getter), m_setter(setter), m_has_changed(false) {
        
        // Default formatter
        m_formatter = [](const T& value) -> std::string {
            if constexpr (std::is_same_v<T, std::string>) {
                return value;
            } else if constexpr (std::is_arithmetic_v<T>) {
                return std::to_string(value);
            } else {
                return "[Complex Type]";
            }
        };
        
        if (m_getter) {
            m_current_value = m_getter();
            m_previous_value = m_current_value;
        }
    }
    
    void Update() override {
        if (m_getter) {
            m_previous_value = m_current_value;
            m_current_value = m_getter();
            m_has_changed = (m_current_value != m_previous_value);
        }
    }
    
    bool HasChanged() const override {
        return m_has_changed;
    }
    
    void ClearChanged() override {
        m_has_changed = false;
    }
    
    std::type_index GetTypeIndex() const override {
        return std::type_index(typeid(T));
    }
    
    const std::any& GetValue() const override {
        m_cached_any = m_current_value;
        return m_cached_any;
    }
    
    void SetValue(const std::any& value) override {
        try {
            T new_value = std::any_cast<T>(value);
            if (!m_validator || m_validator(new_value)) {
                m_previous_value = m_current_value;
                m_current_value = new_value;
                m_has_changed = true;
                
                if (m_setter) {
                    m_setter(new_value);
                }
            }
        } catch (const std::bad_any_cast& e) {
            LUMINUMBRA_CORE_WARN("[UIDataStream] Type mismatch for binding '{}': {}", m_id, e.what());
        }
    }
    
    std::string GetValueAsString() const override {
        return m_formatter ? m_formatter(m_current_value) : "[No Formatter]";
    }
    
    // Typed accessors
    const T& GetTypedValue() const { return m_current_value; }
    const T& GetPreviousValue() const { return m_previous_value; }
    
    void SetValidator(Validator validator) { m_validator = validator; }
    void SetFormatter(Formatter formatter) { m_formatter = formatter; }
    
    void SetDirectValue(const T& value) {
        m_previous_value = m_current_value;
        m_current_value = value;
        m_has_changed = true;
    }

private:
    std::string m_id;
    Getter m_getter;
    Setter m_setter;
    Validator m_validator;
    Formatter m_formatter;
    
    T m_current_value{};
    T m_previous_value{};
    bool m_has_changed;
    mutable std::any m_cached_any; // For type erasure
};

// Stream performance statistics
struct StreamStats {
    size_t total_bindings = 0;
    size_t active_bindings = 0;
    size_t updates_per_frame = 0;
    float average_update_time_ms = 0.0f;
    float max_update_time_ms = 0.0f;
    std::chrono::high_resolution_clock::time_point last_update;
};

// Main data streaming system
class UIDataStream {
public:
    UIDataStream();
    ~UIDataStream();
    
    // Thread-safe binding creation
    template<typename T>
    void BindRealtimeData(const std::string& id, std::function<T()> getter) {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto binding = std::make_unique<TypedDataBinding<T>>(id, getter);
        m_bindings[id] = std::move(binding);
        m_stats.total_bindings++;
        
        LUMINUMBRA_CORE_INFO("[UIDataStream] Bound realtime data: {}", id);
    }
    
    template<typename T>
    void BindBidirectionalData(const std::string& id, 
                              std::function<T()> getter, 
                              std::function<void(const T&)> setter) {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto binding = std::make_unique<TypedDataBinding<T>>(id, getter, setter);
        m_bindings[id] = std::move(binding);
        m_stats.total_bindings++;
        
        LUMINUMBRA_CORE_INFO("[UIDataStream] Bound bidirectional data: {}", id);
    }
    
    // Direct data updates (for high-frequency data)
    template<typename T>
    void UpdateStream(const std::string& id, const T& value) {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto it = m_bindings.find(id);
        if (it != m_bindings.end()) {
            // Check if it's the right type
            auto typed_binding = dynamic_cast<TypedDataBinding<T>*>(it->second.get());
            if (typed_binding) {
                typed_binding->SetDirectValue(value);
                m_dirty_streams.push_back(id);
            } else {
                LUMINUMBRA_CORE_WARN("[UIDataStream] Type mismatch for stream: {}", id);
            }
        } else {
            // Auto-create binding for direct updates
            auto binding = std::make_unique<TypedDataBinding<T>>(id);
            binding->SetDirectValue(value);
            m_dirty_streams.push_back(id);
            m_bindings[id] = std::move(binding);
            m_stats.total_bindings++;
        }
    }
    
    // Batch update interface for performance
    void BeginBatchUpdate() {
        m_batch_mode = true;
        m_batch_updates.clear();
    }
    
    template<typename T>
    void BatchUpdateStream(const std::string& id, const T& value) {
        if (m_batch_mode) {
            m_batch_updates[id] = value;
        } else {
            UpdateStream(id, value);
        }
    }
    
    void EndBatchUpdate() {
        if (!m_batch_mode) return;
        
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        for (const auto& [id, value] : m_batch_updates) {
            auto it = m_bindings.find(id);
            if (it != m_bindings.end()) {
                it->second->SetValue(value);
                m_dirty_streams.push_back(id);
            }
        }
        
        m_batch_mode = false;
        m_batch_updates.clear();
    }
    
    // Access typed data
    template<typename T>
    bool GetValue(const std::string& id, T& out_value) const {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto it = m_bindings.find(id);
        if (it != m_bindings.end()) {
            auto typed_binding = dynamic_cast<TypedDataBinding<T>*>(it->second.get());
            if (typed_binding) {
                out_value = typed_binding->GetTypedValue();
                return true;
            }
        }
        return false;
    }
    
    template<typename T>
    T GetValueOr(const std::string& id, const T& default_value) const {
        T value;
        return GetValue(id, value) ? value : default_value;
    }
    
    // Check if data has changed
    bool HasChanged(const std::string& id) const {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto it = m_bindings.find(id);
        return it != m_bindings.end() ? it->second->HasChanged() : false;
    }
    
    // Type-erased access for generic UI components
    std::string GetValueAsString(const std::string& id) const {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto it = m_bindings.find(id);
        return it != m_bindings.end() ? it->second->GetValueAsString() : "";
    }
    
    // Stream management
    void RemoveStream(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        auto it = m_bindings.find(id);
        if (it != m_bindings.end()) {
            m_bindings.erase(it);
            m_stats.total_bindings--;
            LUMINUMBRA_CORE_INFO("[UIDataStream] Removed stream: {}", id);
        }
    }
    
    void ClearAllStreams() {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        m_bindings.clear();
        m_stats.total_bindings = 0;
        LUMINUMBRA_CORE_INFO("[UIDataStream] Cleared all streams");
    }
    
    std::vector<std::string> GetStreamIds() const {
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        std::vector<std::string> ids;
        ids.reserve(m_bindings.size());
        for (const auto& [id, binding] : m_bindings) {
            ids.push_back(id);
        }
        return ids;
    }
    
    // Performance monitoring
    void FlushUpdates() {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        std::lock_guard<std::mutex> lock(m_bindings_mutex);
        
        m_stats.active_bindings = 0;
        m_stats.updates_per_frame = 0;
        
        for (auto& [id, binding] : m_bindings) {
            binding->Update();
            if (binding->HasChanged()) {
                m_stats.updates_per_frame++;
            }
            m_stats.active_bindings++;
        }
        
        // Clear dirty flags
        for (const auto& id : m_dirty_streams) {
            auto it = m_bindings.find(id);
            if (it != m_bindings.end()) {
                it->second->ClearChanged();
            }
        }
        m_dirty_streams.clear();
        
        auto end_time = std::chrono::high_resolution_clock::now();
        float update_time = std::chrono::duration<float, std::milli>(end_time - start_time).count();
        
        // Update performance stats
        m_stats.max_update_time_ms = std::max(m_stats.max_update_time_ms, update_time);
        m_stats.average_update_time_ms = m_stats.average_update_time_ms * 0.9f + update_time * 0.1f;
        m_stats.last_update = end_time;
    }
    
    const StreamStats& GetStats() const { return m_stats; }
    
    // Debug and monitoring
    void ShowDebugWindow(bool* p_open = nullptr);
    void LogStreamState() const;

private:
    mutable std::mutex m_bindings_mutex;
    std::unordered_map<std::string, std::unique_ptr<IDataBinding>> m_bindings;
    std::vector<std::string> m_dirty_streams;
    
    // Batch update system
    std::atomic<bool> m_batch_mode{false};
    std::unordered_map<std::string, std::any> m_batch_updates;
    
    // Performance tracking
    StreamStats m_stats;
    
    static constexpr size_t MAX_DIRTY_STREAMS = 1000; // Prevent memory bloat
};

// Global data stream instance
class UIDataStreamManager {
public:
    static UIDataStream& GetInstance() {
        static UIDataStream instance;
        return instance;
    }
    
    // Convenience functions for global access
    template<typename T>
    static void Bind(const std::string& id, std::function<T()> getter) {
        GetInstance().BindRealtimeData(id, getter);
    }
    
    template<typename T>
    static void Update(const std::string& id, const T& value) {
        GetInstance().UpdateStream(id, value);
    }
    
    template<typename T>
    static T Get(const std::string& id, const T& default_value = T{}) {
        return GetInstance().GetValueOr(id, default_value);
    }
    
    static bool HasChanged(const std::string& id) {
        return GetInstance().HasChanged(id);
    }
    
    static void Flush() {
        GetInstance().FlushUpdates();
    }
    
private:
    UIDataStreamManager() = default;
    ~UIDataStreamManager() = default;
    UIDataStreamManager(const UIDataStreamManager&) = delete;
    UIDataStreamManager& operator=(const UIDataStreamManager&) = delete;
};

} // namespace Luminumbra::Client::UI