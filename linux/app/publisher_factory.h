#pragma once

#include <memory>

#include "streambridge/media_errors.h"
#include "streambridge/transport.h"
#include "streambridge/transport_config.h"

namespace streambridge {

Result<std::unique_ptr<IMediaPublisher>> create_publisher_for_transport(
    const TransportConfig& config);

}  // namespace streambridge