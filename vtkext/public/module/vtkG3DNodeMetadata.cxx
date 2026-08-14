#include "vtkG3DNodeMetadata.h"

#include <vtkInformation.h>
#include <vtkInformationStringKey.h>
#include <vtkInformationStringVectorKey.h>
#include <vtkObjectFactory.h>

vtkStandardNewMacro(vtkG3DNodeMetadata);

vtkInformationKeyMacro(vtkG3DNodeMetadata, NODE_TYPE, String);
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
