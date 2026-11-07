#ifndef LOGGOPSIM_NODEVALIDATOR_HPP
#define LOGGOPSIM_NODEVALIDATOR_HPP

#include <any>
#include <functional>
#include <stdexcept>
#include <utility>

namespace traceschema {

// A consumer's check of each decoded node, possibly empty, for the output type a reader was
// created for. The type is erased once at setup; get() keeps a cheap identity check, so that
// requesting another output type fails before any bytes are written. Readers own one each and
// are not movable, so neither is this.
class NodeValidator {
public:
    template<class Output>
    explicit NodeValidator(std::function<void(const Output&)> validate)
        : function_(std::move(validate)), type_(&type_key_<Output>) {
        address_ = std::any_cast<std::function<void(const Output&)>>(&function_);
    }
    NodeValidator(const NodeValidator&) = delete;
    NodeValidator& operator=(const NodeValidator&) = delete;

    template<class Output>
    [[nodiscard]] const std::function<void(const Output&)>& get() const {
        if (type_ != &type_key_<Output>)
            throw std::invalid_argument("Reader has no bindings for this output type");
        return *static_cast<const std::function<void(const Output&)>*>(address_);
    }

private:
    template<class Output>
    inline static constexpr unsigned char type_key_ = 0;

    std::any function_; // Owns std::function<void(const Output&)>.
    const void* address_ = nullptr;
    const void* type_ = nullptr;
};

} // namespace traceschema
#endif
