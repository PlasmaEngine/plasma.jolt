#include <JoltPlugin/JoltPluginPCH.h>

#include <JoltPlugin/Components/JoltRagdollMessages.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plJoltRagdollBalanceState, 1)
  PL_ENUM_CONSTANTS(plJoltRagdollBalanceState::Standing, plJoltRagdollBalanceState::Stepping, plJoltRagdollBalanceState::Falling, plJoltRagdollBalanceState::Down)
PL_END_STATIC_REFLECTED_ENUM;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgRagdollImpact);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgRagdollImpact, 1, plRTTIDefaultAllocator<plMsgRagdollImpact>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Region", plJoltRagdollRegion, m_Region),
    PL_MEMBER_PROPERTY("Position", m_vPosition),
    PL_MEMBER_PROPERTY("Normal", m_vNormal),
    PL_MEMBER_PROPERTY("Impulse", m_fImpulse),
    PL_MEMBER_PROPERTY("Sliding", m_bSliding),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgRagdollJointDislocated);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgRagdollJointDislocated, 1, plRTTIDefaultAllocator<plMsgRagdollJointDislocated>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("JointName", m_sJointName),
    PL_ENUM_MEMBER_PROPERTY("Region", plJoltRagdollRegion, m_Region),
    PL_MEMBER_PROPERTY("Position", m_vPosition),
    PL_MEMBER_PROPERTY("Speed", m_fSpeed),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_IMPLEMENT_MESSAGE_TYPE(plMsgRagdollBalanceLost);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMsgRagdollBalanceLost, 1, plRTTIDefaultAllocator<plMsgRagdollBalanceLost>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Direction", m_vDirection),
    PL_MEMBER_PROPERTY("Speed", m_fSpeed),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_STATICLINK_FILE(JoltPlugin, JoltPlugin_Components_Implementation_JoltRagdollMessages);
