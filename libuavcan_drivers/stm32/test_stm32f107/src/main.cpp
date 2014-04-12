/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#include <unistd.h>
#include <crdr_chibios/sys/sys.h>
#include <uavcan_stm32/uavcan_stm32.hpp>
#include <uavcan/protocol/global_time_sync_slave.hpp>

namespace app
{
namespace
{

uavcan_stm32::CanInitHelper<128> can;

typedef uavcan::Node<16384> Node;

uavcan::LazyConstructor<Node> node_;

Node& getNode()
{
    if (!node_.isConstructed())
    {
        node_.construct<uavcan::ICanDriver&, uavcan::ISystemClock&>(can.driver, uavcan_stm32::SystemClock::instance());
    }
    return *node_;
}

void ledSet(bool state)
{
    palWritePad(GPIO_PORT_LED, GPIO_PIN_LED, state);
}

int init()
{
    int res = 0;

    halInit();
    chibios_rt::System::init();
    sdStart(&STDOUT_SD, NULL);

    res = can.init(1000000);
    if (res < 0)
    {
        goto leave;
    }

leave:
    return res;
}

__attribute__((noreturn))
void die(int status)
{
    lowsyslog("Now I am dead x_x %i\n", status);
    while (1)
    {
        ledSet(false);
        sleep(1);
        ledSet(true);
        sleep(1);
    }
}

class : public chibios_rt::BaseStaticThread<8192>
{
public:
    msg_t main()
    {
        /*
         * Setting up the node parameters
         */
        Node& node = app::getNode();

        node.setNodeID(64);
        node.setName("org.uavcan.stm32_test_stm32f107");

        /*
         * Initializing the UAVCAN node - this may take a while
         */
        while (true)
        {
            // Calling start() multiple times is OK - only the first successfull call will be effective
            int res = node.start();

            uavcan::NetworkCompatibilityCheckResult ncc_result;
            if (res >= 0)
            {
                lowsyslog("Checking network compatibility...\n");
                res = node.checkNetworkCompatibility(ncc_result);
            }

            if (res < 0)
            {
                lowsyslog("Node initialization failure: %i, will try agin soon\n", res);
            }
            else if (!ncc_result.isOk())
            {
                lowsyslog("Network conflict with %u, will try again soon\n", ncc_result.conflicting_node.get());
            }
            else
            {
                break;
            }
            ::sleep(3);
        }

        /*
         * Time synchronizer
         */
        static uavcan::GlobalTimeSyncSlave time_sync_slave(node);
        {
            const int res = time_sync_slave.start();
            if (res < 0)
            {
                die(res);
            }
        }

        /*
         * Main loop
         */
        lowsyslog("UAVCAN node started\n");
        node.setStatusOk();
        node.getLogger().setLevel(uavcan::protocol::debug::LogLevel::INFO);
        while (true)
        {
            const int spin_res = node.spin(uavcan::MonotonicDuration::fromMSec(5000));
            if (spin_res < 0)
            {
                lowsyslog("Spin failure: %i\n", spin_res);
            }

            lowsyslog("Time sync master: %u\n", unsigned(time_sync_slave.getMasterNodeID().get()));

            lowsyslog("Memory usage: used=%u free=%u\n",
                      node.getAllocator().getNumUsedBlocks(), node.getAllocator().getNumFreeBlocks());

            lowsyslog("CAN errors: %lu %lu\n",
                      static_cast<unsigned long>(can.driver.getIface(0)->getErrorCount()),
                      static_cast<unsigned long>(can.driver.getIface(1)->getErrorCount()));

            node.logInfo("app", "UTC %* sec, %* corr, %* jumps",
                         uavcan_stm32::clock::getUtc().toMSec() / 1000,
                         uavcan_stm32::clock::getUtcSpeedCorrectionPPM(),
                         uavcan_stm32::clock::getUtcAjdustmentJumpCount());
        }
        return msg_t();
    }
} uavcan_node_thread;

}
}

int main()
{
    const int init_res = app::init();
    if (init_res != 0)
    {
        app::die(init_res);
    }

    lowsyslog("Starting the UAVCAN thread\n");
    app::uavcan_node_thread.start(LOWPRIO);

    while (true)
    {
        for (int i = 0; i < 200; i++)
        {
            app::ledSet(app::can.driver.hadActivity());
            ::usleep(25000);
        }

        const uavcan::UtcTime utc = uavcan_stm32::clock::getUtc();
        lowsyslog("UTC %lu sec   Absolute correction: %li usec   Speed correction: %liPPM   Jumps: %lu\n",
                  static_cast<unsigned long>(utc.toMSec() / 1000),
                  static_cast<long>(uavcan_stm32::clock::getPrevUtcAdjustment().toUSec()),
                  uavcan_stm32::clock::getUtcSpeedCorrectionPPM(),
                  uavcan_stm32::clock::getUtcAjdustmentJumpCount());
    }
}
