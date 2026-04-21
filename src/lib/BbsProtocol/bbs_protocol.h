/**
 * BBS refers to "Bit-bang serial". 
 * 
 * The protocol implents the "SPI" mode for the RX5808 VRX modules. However, the protocol 
 * does  not conform to the standard hardware SPI protocol. It instead uses code for timing 
 * and transmission. And therefore, I call it "Bit-bang serial".
 */

#pragma once

#include <stdint.h>

namespace BbsProtocol {
  void setVrxChannel(uint16_t channel);
  void setPowerDownRegister(uint32_t value);
};
