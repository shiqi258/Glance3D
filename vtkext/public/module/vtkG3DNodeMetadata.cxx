#include "vtkG3DNodeMetadata.h"

#include <vtkDataAssembly.h>
#include <vtkInformation.h>
#include <vtkInformationIntegerKey.h>
#include <vtkInformationStringKey.h>
#include <vtkInformationStringVectorKey.h>
#include <vtkObjectFactory.h>

#include <algorithm>
#include <cctype>
#include <string_view>

vtkStandardNewMacro(vtkG3DNodeMetadata);

vtkInformationKeyMacro(vtkG3DNodeMetadata, NODE_TYPE, String);
vtkInformationKeyMacro(vtkG3DNodeMetadata, INSTANCE_TARGET, String);
vtkInformationKeyMacro(vtkG3DNodeMetadata, FACE_COUNT, Integer);
vtkInformationKeyMacro(vtkG3DNodeMetadata, PROPERTIES, StringVector);

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetNodeType(vtkInformation* info, const std::string& type)
{
  if (info == nullptr || type.empty())
  {
    return;
  }
  info->Set(vtkG3DNodeMetadata::NODE_TYPE(), type);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetInstanceTarget(vtkInformation* info, const std::string& productName)
{
  if (info == nullptr || productName.empty())
  {
    return;
  }
  info->Set(vtkG3DNodeMetadata::INSTANCE_TARGET(), productName);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetFaceCount(vtkInformation* info, int count)
{
  // Zero means "this node has no B-rep behind it", which is also what an absent key means, so it is
  // not recorded -- a node should not read as face-capable-but-empty.
  if (info == nullptr || count <= 0)
  {
    return;
  }
  info->Set(vtkG3DNodeMetadata::FACE_COUNT(), count);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::AddProperty(
  vtkInformation* info, const std::string& key, const std::string& value)
{
  // A property with no name could never be shown or looked up, and would shift every later pair by
  // one if it were stored, so it is dropped rather than half-recorded.
  if (info == nullptr || key.empty())
  {
    return;
  }

  vtkInformationStringVectorKey* properties = vtkG3DNodeMetadata::PROPERTIES();
  const int length = info->Length(properties);
  info->Set(properties, key.c_str(), length);
  info->Set(properties, value.c_str(), length + 1);
}

//----------------------------------------------------------------------------
std::string vtkG3DNodeMetadata::GetNodeType(vtkInformation* info)
{
  if (info == nullptr || !info->Has(vtkG3DNodeMetadata::NODE_TYPE()))
  {
    return {};
  }
  const char* type = info->Get(vtkG3DNodeMetadata::NODE_TYPE());
  return type ? std::string(type) : std::string();
}

//----------------------------------------------------------------------------
std::string vtkG3DNodeMetadata::GetInstanceTarget(vtkInformation* info)
{
  if (info == nullptr || !info->Has(vtkG3DNodeMetadata::INSTANCE_TARGET()))
  {
    return {};
  }
  const char* target = info->Get(vtkG3DNodeMetadata::INSTANCE_TARGET());
  return target ? std::string(target) : std::string();
}

//----------------------------------------------------------------------------
int vtkG3DNodeMetadata::GetFaceCount(vtkInformation* info)
{
  if (info == nullptr || !info->Has(vtkG3DNodeMetadata::FACE_COUNT()))
  {
    return 0;
  }
  return info->Get(vtkG3DNodeMetadata::FACE_COUNT());
}

//----------------------------------------------------------------------------
std::vector<std::pair<std::string, std::string>> vtkG3DNodeMetadata::GetProperties(
  vtkInformation* info)
{
  std::vector<std::pair<std::string, std::string>> properties;
  if (info == nullptr)
  {
    return properties;
  }

  vtkInformationStringVectorKey* key = vtkG3DNodeMetadata::PROPERTIES();
  const int length = info->Length(key);
  properties.reserve(static_cast<std::size_t>(length / 2));
  // Step by two and stop one short of an odd tail: a truncated pair is dropped rather than paired
  // with an empty value it never had.
  for (int index = 0; index + 1 < length; index += 2)
  {
    const char* name = info->Get(key, index);
    const char* value = info->Get(key, index + 1);
    if (name != nullptr)
    {
      properties.emplace_back(std::string(name), value ? std::string(value) : std::string());
    }
  }
  return properties;
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyLabel(
  vtkDataAssembly* assembly, int nodeId, const std::string& label)
{
  if (assembly == nullptr || label.empty())
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::Label, label.c_str());
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyNodeType(
  vtkDataAssembly* assembly, int nodeId, const std::string& type)
{
  if (assembly == nullptr || type.empty())
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::NodeType, type.c_str());
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyInstanceTarget(
  vtkDataAssembly* assembly, int nodeId, const std::string& productName)
{
  if (assembly == nullptr || productName.empty())
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::InstanceTarget, productName.c_str());
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyFaceCount(vtkDataAssembly* assembly, int nodeId, int count)
{
  if (assembly == nullptr || count <= 0)
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::FaceCount, count);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::AddAssemblyProperty(
  vtkDataAssembly* assembly, int nodeId, const std::string& key, const std::string& value)
{
  if (assembly == nullptr || key.empty())
  {
    return;
  }

  // The count doubles as the next free slot, so appending never has to scan for one.
  const int index = assembly->GetAttributeOrDefault(nodeId, G3DAssemblyAttribute::PropertyCount, 0);
  const std::string suffix = std::to_string(index);
  assembly->SetAttribute(
    nodeId, (G3DAssemblyAttribute::PropertyKeyPrefix + suffix).c_str(), key.c_str());
  assembly->SetAttribute(
    nodeId, (G3DAssemblyAttribute::PropertyValuePrefix + suffix).c_str(), value.c_str());
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::PropertyCount, index + 1);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyFlatActorId(
  vtkDataAssembly* assembly, int nodeId, int flatActorId)
{
  if (assembly == nullptr || flatActorId < 0)
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::FlatActorId, flatActorId);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyCameraIndex(vtkDataAssembly* assembly, int nodeId, int localIndex)
{
  if (assembly == nullptr || localIndex < 0)
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::CameraIndex, localIndex);
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::SetAssemblyLightIndex(vtkDataAssembly* assembly, int nodeId, int localIndex)
{
  if (assembly == nullptr || localIndex < 0)
  {
    return;
  }
  assembly->SetAttribute(nodeId, G3DAssemblyAttribute::LightIndex, localIndex);
}

//----------------------------------------------------------------------------
bool vtkG3DNodeMetadata::IsGeneratedNodeName(const std::string& name)
{
  static constexpr std::string_view prefixes[] = { "node", "object", "actor_", "primitive_" };
  for (const std::string_view prefix : prefixes)
  {
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
    {
      continue;
    }
    if (std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
          [](unsigned char character) { return std::isdigit(character) != 0; }))
    {
      return true;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
void vtkG3DNodeMetadata::ForwardToAssembly(
  vtkDataAssembly* assembly, int nodeId, vtkInformation* blockInfo)
{
  if (assembly == nullptr || blockInfo == nullptr)
  {
    return;
  }

  vtkG3DNodeMetadata::SetAssemblyNodeType(
    assembly, nodeId, vtkG3DNodeMetadata::GetNodeType(blockInfo));
  vtkG3DNodeMetadata::SetAssemblyInstanceTarget(
    assembly, nodeId, vtkG3DNodeMetadata::GetInstanceTarget(blockInfo));
  vtkG3DNodeMetadata::SetAssemblyFaceCount(
    assembly, nodeId, vtkG3DNodeMetadata::GetFaceCount(blockInfo));

  for (const auto& property : vtkG3DNodeMetadata::GetProperties(blockInfo))
  {
    vtkG3DNodeMetadata::AddAssemblyProperty(assembly, nodeId, property.first, property.second);
  }
}
