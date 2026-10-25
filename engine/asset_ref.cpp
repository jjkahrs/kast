#include "asset_ref.h"

namespace kast {
namespace {

AssetRefHooks& hooks()
{
    static AssetRefHooks value;
    return value;
}

} // namespace

void setAssetRefHooks(AssetRefHooks value) { hooks() = std::move(value); }

const AssetRefHooks& assetRefHooks() { return hooks(); }

} // namespace kast
