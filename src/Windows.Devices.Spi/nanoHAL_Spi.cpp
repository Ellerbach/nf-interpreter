//
// Copyright (c) 2020 The nanoFramework project contributors
// Portions Copyright (c) Microsoft Corporation.  All rights reserved.
// See LICENSE file in the project root for full license information.
//
//
// nano_SPI_xxxx
//
// Logical SPI device access
//
#include <targetPAL.h>

//#include <targetHAL.h>
#include <nanoCLR_Interop.h>
#include <nanoCLR_Runtime.h>
#include <nanoCLR_Checks.h>

#define CreateSpiHandle(spiBus, deviceIndex) ((CPU_DEVICE_TYPE_SPI << 16) + (spiBus << 8) + deviceIndex)

#define GetBusFromHandle(handle) ((handle >> 8) & 0x00ff);

// Saved config for each available SPI bus
nanoSPI_BusConfig spiconfig[NUM_SPI_BUSES];

// Define weak functions for some optional PAL functions
// so low level implementations don't require them.
__nfweak bool CPU_SPI_Initialize(uint8_t bus)
{
    (void)bus;
    return true;
}
__nfweak void CPU_SPI_Uninitialize(uint8_t bus)
{
    (void)bus;
}
__nfweak SPI_OP_STATUS CPU_SPI_Op_Status(uint32_t deviceHandle)
{
    (void)deviceHandle;
    return SPI_OP_STATUS::SPI_OP_COMPLETE;
}

__nfweak uint32_t CPU_SPI_Add_Device(const SPI_DEVICE_CONFIGURATION &spiDeviceConfig)
{
    (void)spiDeviceConfig;
    return 1;
}

__nfweak bool CPU_SPI_Remove_Device(uint32_t deviceHandle)
{
    (void)deviceHandle;
    return true;
};

// Number of SPI buses available
__nfweak uint32_t CPU_SPI_PortsCount()
{
    uint32_t count = 0;
    uint32_t map = CPU_SPI_PortsMap();

    while (map > 0)
    {
        if (map & 0x01)
            count++;
        map >>= 1;
    }
    return count;
}

// Pins not know
__nfweak void CPU_SPI_GetPins(uint32_t spi_bus, GPIO_PIN &clockPin, GPIO_PIN &misoPin, GPIO_PIN &mosiPin)
{
    (void)spi_bus;

    clockPin = GPIO_PIN_NONE;
    misoPin = GPIO_PIN_NONE;
    mosiPin = GPIO_PIN_NONE;
}

//
//  Find the Bus / Device ptr base on handle
//
//  return true = handle valid
static bool getDevice(uint32_t handle, uint8_t &spiBus, int &deviceIndex)
{
    int type = handle >> 16 & 0x00ff;
    deviceIndex = handle & 0x00ff;

    spiBus = GetBusFromHandle(handle);

    // Validate type, bus, deviceIndex
    if (type != CPU_DEVICE_TYPE_SPI || spiBus >= CPU_SPI_PortsCount() || deviceIndex >= NUM_SPI_BUSES)
        return false;

    return true;
}

// Find a free slot in the device table
// Return index or -1 if no free slots
static int FindFreeDeviceSlotSpi(int spiBus, GPIO_PIN cs)
{
    for (int deviceIndex = 0; deviceIndex < MAX_SPI_DEVICES; deviceIndex++)
    {
        if (spiconfig[spiBus].deviceHandles[deviceIndex] == 0)
            return deviceIndex;
        // Check device chip select not already in use
        if (spiconfig[spiBus].deviceCongfig[deviceIndex].DeviceChipSelect == cs)
            return -2;
    }
    return -1;
}

// Initialise the spiconfig structure
// Called on CLR startup
bool nanoSPI_Initialize()
{
    for (int spiBus = 0; spiBus < NUM_SPI_BUSES; spiBus++)
    {
        spiconfig[spiBus].spiBusInited = false;
        spiconfig[spiBus].devicesInUse = 0;
        memset(&spiconfig[spiBus].deviceHandles, 0, sizeof(spiconfig[spiBus].deviceHandles));
    }
    return true;
}

// Uninitializes (resets) all SPI devices.
// Called on CLR closedown
void nanoSPI_Uninitialize()
{
    for (int spiBus = 0; spiBus < NUM_SPI_BUSES; spiBus++)
    {
        if (spiconfig[spiBus].spiBusInited)
        {
            // Remove any devices
            // Bus will be closed when last device closed in SPI_CloseDevice
            for (int deviceIndex = 0; deviceIndex < MAX_SPI_DEVICES; deviceIndex++)
            {
                uint32_t deviceHandle = spiconfig[spiBus].deviceHandles[deviceIndex];
                if (deviceHandle != 0)
                    nanoSPI_CloseDevice(CreateSpiHandle(spiBus, deviceIndex));
            }
        }
    }
}

// Open SPI bus / device using device configuration
// Register GPIO pins as in use
// Return handle ( index to device on bus), negative = error
HRESULT nanoSPI_OpenDevice(SPI_DEVICE_CONFIGURATION &Configuration)
{
    return nanoSPI_OpenDeviceEx(Configuration, GPIO_PIN_NONE, GPIO_PIN_NONE, GPIO_PIN_NONE);
}

// Reserve SPI bus pins
HRESULT nanoSPI_ReserveBusPins(int spiBus, bool reserve)
{
    GPIO_PIN pins[3];

    CPU_SPI_GetPins(spiBus, pins[0], pins[1], pins[2]);

    if (reserve)
    {
        // if reserve , Check can reserve
        for (int i = 0; i < 3; i++)
        {
            if (pins[0] != GPIO_PIN_NONE)
            {
                if (CPU_GPIO_PinIsBusy(pins[i]))
                    return CLR_E_INVALID_PARAMETER;
            }
        }
    }

    // Reserve / UnReserve pins
    for (int i = 0; i < 3; i++)
    {
        if (CPU_GPIO_ReservePin(pins[i], reserve) == false)
            return CLR_E_INVALID_PARAMETER;
    }

    return S_OK;
}

// Specify alternate pins for SPI or -1 for use default for bus
HRESULT nanoSPI_OpenDeviceEx(
    SPI_DEVICE_CONFIGURATION &spiDeviceConfig,
    GPIO_PIN altMsk,
    GPIO_PIN altMiso,
    GPIO_PIN altMosi)
{
    // Use alternate pins if defined
    // TODO params
    (void)altMsk;
    (void)altMiso;
    (void)altMosi;

    int deviceIndex;
    // spiBus 0 to (number of buses - 1)
    uint8_t spiBus = (uint8_t)spiDeviceConfig.Spi_Bus;
    int32_t deviceHandle;

    // Validate Bus available
    if (!(CPU_SPI_PortsMap() & (1 << spiBus)))
        return CLR_E_INVALID_PARAMETER;

    // Check not maximum devices per SPI bus reached
    if (spiconfig[spiBus].devicesInUse >= MAX_SPI_DEVICES)
        return CLR_E_INDEX_OUT_OF_RANGE;

    // Initialise Bus if not already initialised
    if (!spiconfig[spiBus].spiBusInited)
    {
        if (!CPU_SPI_Initialize(spiBus))
            return CLR_E_INVALID_PARAMETER;

        // Reserve pins used by SPI bus
        HRESULT hr = nanoSPI_ReserveBusPins(spiBus, true);
        if (hr != S_OK)
            return hr;

        spiconfig[spiBus].spiBusInited = true;
    }

    // Find if device slot is available and check
    deviceIndex = FindFreeDeviceSlotSpi(spiBus, spiDeviceConfig.DeviceChipSelect);
    if (deviceIndex < 0)
    {
        if (deviceIndex == -1)
            // No device slots left
            return CLR_E_INDEX_OUT_OF_RANGE;
        else
            // Return NOT_SUPPORTED when Device already in use. Not really any other relevant exception that's
            // currently raised in managed code
            return CLR_E_NOT_SUPPORTED;
    }

    // Add device and get handle
    deviceHandle = CPU_SPI_Add_Device(spiDeviceConfig);
    if (deviceHandle != 0)
    {
        // Reserve chip select pin
        if (CPU_GPIO_ReservePin((GPIO_PIN)spiDeviceConfig.DeviceChipSelect, true) == false)
        {
            // Failed to reserve CS pin
            CPU_SPI_Remove_Device(deviceHandle);
            return CLR_E_FAIL;
        }
    }

    // Add next Device - Copy device config, save handle, increment number devices on bus
    nanoSPI_BusConfig *pBusConfig = &spiconfig[spiBus];
    pBusConfig->deviceCongfig[deviceIndex] = spiDeviceConfig;
    pBusConfig->deviceHandles[deviceIndex] = deviceHandle;

    // Compute rough estimate on the time to tx/rx a byte (in milliseconds)
    // Used to compute length of time for each IO to see if this is a long running operation
    pBusConfig->byteTime = (1.0 / spiDeviceConfig.Clock_RateHz) * 1000;

    pBusConfig->devicesInUse++;

    // Return unique generated handle
    return CreateSpiHandle(spiBus, deviceIndex);
}

//  SPI_CloseDevice
//
//  Close device on spi bus
//
HRESULT nanoSPI_CloseDevice(uint32_t handle)
{
    uint8_t spiBus;
    int deviceIndex;

    if (!getDevice(handle, spiBus, deviceIndex))
        return CLR_E_INVALID_PARAMETER;

    // Remove device from bus (ignore any error )
    CPU_SPI_Remove_Device(spiconfig[spiBus].deviceHandles[deviceIndex]);

    spiconfig[spiBus].deviceHandles[deviceIndex] = 0;
    spiconfig[spiBus].devicesInUse--;

    // Unreserve CS pin
    CPU_GPIO_ReservePin(spiconfig[spiBus].deviceCongfig->DeviceChipSelect, false);

    // Last device on bus then close bus and also remove bus pin reserves
    if (spiconfig[spiBus].devicesInUse <= 0)
    {
        // Uninitialise bus and reset init flag
        CPU_SPI_Uninitialize(spiBus);
        spiconfig[spiBus].spiBusInited = false;

        // Unreserve bus pins
        nanoSPI_ReserveBusPins(spiBus, false);
    }

    return S_OK;
}

//
// Get time to send 1 byte on bus using current config
//
float nanoSPI_GetByteTime(uint32_t handle)
{
    uint8_t spiBus = GetBusFromHandle(handle);

    return spiconfig[spiBus].byteTime;
}

//
// Write and/or read data to device on SPi bus
//
HRESULT nanoSPI_Write_Read(
    uint32_t handle,
    SPI_WRITE_READ_SETTINGS &swrs,
    uint8_t *writePtr,
    int32_t wsize,
    uint8_t *readPtr,
    int32_t readSize)
{
    uint8_t spiBus;
    int deviceIndex;

    if (!getDevice(handle, spiBus, deviceIndex))
        return CLR_E_INVALID_PARAMETER;

    return CPU_SPI_nWrite_nRead(
        spiconfig[spiBus].deviceHandles[deviceIndex],
        spiconfig[spiBus].deviceCongfig[deviceIndex],
        swrs,
        writePtr,
        wsize,
        readPtr,
        readSize);
}

SPI_OP_STATUS nanoSPI_Op_Status(uint32_t handle)
{
    uint8_t spiBus;
    int deviceIndex;

    getDevice(handle, spiBus, deviceIndex);
    return CPU_SPI_Op_Status(spiconfig[spiBus].deviceHandles[deviceIndex]);
}
