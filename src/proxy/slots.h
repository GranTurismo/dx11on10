// Vtable slot numbers of the COM methods we hook, taken from the SDK headers.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Slots {
    int devCreateBuffer, devCreateTexture1D, devCreateTexture2D, devCreateTexture3D, devCreateShaderResourceView;
    int devCreateUnorderedAccessView, devCreateDepthStencilView, devCreateInputLayout;
    int devCreateVertexShader, devCreateGeometryShader, devCreateGeometryShaderWithStreamOutput;
    int devCreatePixelShader, devCreateHullShader, devCreateDomainShader, devCreateComputeShader;
    int devCreateBlendState, devCreateDeferredContext, devCheckFeatureSupport, devGetFeatureLevel;
    int devGetImmediateContext, dev1GetImmediateContext1, dev2GetImmediateContext2, dev3GetImmediateContext3;
    int dev1CreateDeferredContext1, dev1CreateBlendState1, dev2CreateDeferredContext2, dev3CreateDeferredContext3;

    int ctxVSSetShader, ctxPSSetShader, ctxGSSetShader, ctxHSSetShader, ctxDSSetShader, ctxCSSetShader;
    int ctxIASetPrimitiveTopology;
    int ctxDraw, ctxDrawIndexed, ctxDrawInstanced, ctxDrawIndexedInstanced, ctxDrawAuto;
    int ctxDrawInstancedIndirect, ctxDrawIndexedInstancedIndirect, ctxDispatch, ctxDispatchIndirect;
    int ctxCSSetUnorderedAccessViews, ctxOMSetRenderTargetsAndUnorderedAccessViews;
    int ctxClearUnorderedAccessViewUint, ctxClearUnorderedAccessViewFloat, ctxCopyStructureCount;
    int ctxClearState, ctxFinishCommandList, ctxExecuteCommandList;
    int ctxOMSetRenderTargets, ctxClearDepthStencilView, ctxCopyResource, ctxCopySubresourceRegion;
    int ctxVSSetShaderResources, ctxPSSetShaderResources, ctxGSSetShaderResources, ctxCSSetShaderResources;

    int factoryCreateSwapChain, factory2CreateSwapChainForHwnd, factory2CreateSwapChainForCoreWindow;
    int factory2CreateSwapChainForComposition;
    int swapPresent, swap1Present1;
};

extern const struct Slots g_slots;

#ifdef __cplusplus
}
#endif
