#pragma once

#include <Core/Messages/EventMessage.h>
#include <JoltPlugin/JoltPluginDLL.h>
#include <JoltPlugin/Resources/JoltRagdollProfileResource.h>

/// \brief What the balance layer of an active ragdoll is doing.
struct PL_JOLTPLUGIN_DLL plJoltRagdollBalanceState
{
  using StorageType = plUInt8;

  enum Enum
  {
    Standing, ///< Keeping the centre of mass over the feet
    Stepping, ///< Taking a recovery step
    Falling,  ///< Gave up; fall pose and face protection take over
    Down,     ///< The torso is on the ground

    Default = Standing
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_JOLTPLUGIN_DLL, plJoltRagdollBalanceState);

/// \brief Sent by a ragdoll when one of its bodies hits something hard enough, rate-limited per body. Meant for impact sounds and effects.
struct PL_JOLTPLUGIN_DLL plMsgRagdollImpact : public plEventMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgRagdollImpact, plEventMessage);

  plEnum<plJoltRagdollRegion> m_Region;
  plVec3 m_vPosition = plVec3::MakeZero();
  plVec3 m_vNormal = plVec3::MakeZero();
  float m_fImpulse = 0.0f; ///< N·s
  bool m_bSliding = false; ///< the body is moving along the surface rather than into it
};

/// \brief Sent when a joint of a ragdoll dislocates (Injury behaviour).
struct PL_JOLTPLUGIN_DLL plMsgRagdollJointDislocated : public plEventMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgRagdollJointDislocated, plEventMessage);

  plHashedString m_sJointName;
  plEnum<plJoltRagdollRegion> m_Region;
  plVec3 m_vPosition = plVec3::MakeZero();
  float m_fSpeed = 0.0f; ///< deg/s at the limit
};

/// \brief Sent once when the balance layer of an active ragdoll gives up.
struct PL_JOLTPLUGIN_DLL plMsgRagdollBalanceLost : public plEventMessage
{
  PL_DECLARE_MESSAGE_TYPE(plMsgRagdollBalanceLost, plEventMessage);

  plVec3 m_vDirection = plVec3::MakeZero(); ///< the horizontal direction the body falls in
  float m_fSpeed = 0.0f;                    ///< horizontal speed of the centre of mass
};
