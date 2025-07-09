#include "app_task.h"
#include "app/task_executor.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>

using namespace ::chip;
using namespace ::chip::app::Clusters;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath, uint8_t type,
                                       uint16_t size, uint8_t * value)
{
    if (attributePath.mClusterId != OnOff::Id || attributePath.mAttributeId != OnOff::Attributes::OnOff::Id)
        return;

    if (*value) {
        Nrf::PostTask([] { AppTask::SensorActivateHandler(); });
    } else {
        Nrf::PostTask([] { AppTask::SensorDeactivateHandler(); });
    }
}