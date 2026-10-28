/*
 * Copyright 2026 for the Wine project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>

#include "dcomp_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

/* A transform object is handed to IDCompositionVisual::SetTransformObject() and read back when the
 * visual is composited. There is no public way to query the matrix a transform represents, so the
 * objects dcomp creates implement a private interface that gives access to it. The private
 * interface always starts with the IUnknown layout, so that it can be queried for with the object
 * pointer and released independently. */

static const GUID IID_ICompositionTransformPrivate =
    {0x7c1f9c2e, 0x0b3e, 0x4a55, {0x9f, 0x1e, 0x5b, 0x7a, 0x2d, 0x3e, 0x6f, 0x10}};

struct composition_transform;
struct composition_transform_vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct composition_transform *transform, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(struct composition_transform *transform);
    ULONG (STDMETHODCALLTYPE *Release)(struct composition_transform *transform);
    HRESULT (STDMETHODCALLTYPE *GetMatrix)(struct composition_transform *transform, D2D_MATRIX_3X2_F *matrix);
    D2D_MATRIX_3X2_F (STDMETHODCALLTYPE *compute_matrix)(struct composition_transform *transform);
    void (STDMETHODCALLTYPE *destroy)(struct composition_transform *transform);
};

struct composition_transform
{
    const struct composition_transform_vtbl *lpVtbl;
    LONG ref;
};

struct composition_translate_transform
{
    struct composition_transform transform;
    IDCompositionTranslateTransform IDCompositionTranslateTransform_iface;
    float offset_x;
    float offset_y;
};

struct composition_scale_transform
{
    struct composition_transform transform;
    IDCompositionScaleTransform IDCompositionScaleTransform_iface;
    float scale_x;
    float scale_y;
    float center_x;
    float center_y;
};

struct composition_rotate_transform
{
    struct composition_transform transform;
    IDCompositionRotateTransform IDCompositionRotateTransform_iface;
    float angle;
    float center_x;
    float center_y;
};

struct composition_skew_transform
{
    struct composition_transform transform;
    IDCompositionSkewTransform IDCompositionSkewTransform_iface;
    float angle_x;
    float angle_y;
    float center_x;
    float center_y;
};

struct composition_matrix_transform
{
    struct composition_transform transform;
    IDCompositionMatrixTransform IDCompositionMatrixTransform_iface;
    D2D_MATRIX_3X2_F matrix;
};

struct composition_transform_group
{
    struct composition_transform transform;
    IDCompositionTransform IDCompositionTransform_iface;
    struct composition_transform **transforms;
    UINT elements;
};

static void composition_transform_add_ref(struct composition_transform *transform)
{
    InterlockedIncrement(&transform->ref);
}

void composition_transform_release(struct composition_transform *transform)
{
    if (InterlockedDecrement(&transform->ref))
        return;

    transform->lpVtbl->destroy(transform);
}

HRESULT composition_transform_get_matrix(struct composition_transform *transform,
                                         D2D_MATRIX_3X2_F *matrix)
{
    *matrix = transform->lpVtbl->compute_matrix(transform);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE transform_private_QueryInterface(struct composition_transform *transform,
        REFIID iid, void **out)
{
    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate) || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(transform);
        *out = transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE transform_private_AddRef(struct composition_transform *transform)
{
    return InterlockedIncrement(&transform->ref);
}

static ULONG STDMETHODCALLTYPE transform_private_Release(struct composition_transform *transform)
{
    ULONG ref = InterlockedDecrement(&transform->ref);

    if (!ref)
        transform->lpVtbl->destroy(transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE transform_private_GetMatrix(struct composition_transform *transform,
        D2D_MATRIX_3X2_F *matrix)
{
    return composition_transform_get_matrix(transform, matrix);
}

/* Returns a reference to the private transform interface of an object created by dcomp, or NULL if
 * the object comes from another DirectComposition implementation. */
struct composition_transform *composition_transform_from_object(IDCompositionTransform *object)
{
    struct composition_transform *transform = NULL;

    if (!object)
        return NULL;

    if (FAILED(((IUnknown *)object)->lpVtbl->QueryInterface((IUnknown *)object,
                                                           &IID_ICompositionTransformPrivate,
                                                           (void **)&transform)))
    {
        FIXME("Transform object %p is not implemented by dcomp, ignoring.\n", object);
        return NULL;
    }

    return transform;
}

static D2D_MATRIX_3X2_F matrix_identity(void)
{
    D2D_MATRIX_3X2_F matrix = {. _11 = 1.0f, ._22 = 1.0f};

    return matrix;
}

/* Matrices are applied to row vectors, so multiplying a by b applies a first. */
static D2D_MATRIX_3X2_F matrix_multiply(const D2D_MATRIX_3X2_F *a, const D2D_MATRIX_3X2_F *b)
{
    D2D_MATRIX_3X2_F matrix;

    matrix._11 = a->_11 * b->_11 + a->_12 * b->_21;
    matrix._12 = a->_11 * b->_12 + a->_12 * b->_22;
    matrix._21 = a->_21 * b->_11 + a->_22 * b->_21;
    matrix._22 = a->_21 * b->_12 + a->_22 * b->_22;
    matrix._31 = a->_31 * b->_11 + a->_32 * b->_21 + b->_31;
    matrix._32 = a->_31 * b->_12 + a->_32 * b->_22 + b->_32;
    return matrix;
}

static D2D_MATRIX_3X2_F matrix_around_center(const D2D_MATRIX_3X2_F *matrix, float center_x,
                                             float center_y)
{
    D2D_MATRIX_3X2_F translate_to_center = matrix_identity();
    D2D_MATRIX_3X2_F translate_from_center = matrix_identity();
    D2D_MATRIX_3X2_F translated, result;

    translate_to_center._31 = center_x;
    translate_to_center._32 = center_y;
    translate_from_center._31 = -center_x;
    translate_from_center._32 = -center_y;

    translated = matrix_multiply(matrix, &translate_from_center);
    result = matrix_multiply(&translate_to_center, &translated);
    return result;
}

static D2D_MATRIX_3X2_F matrix_translate(float offset_x, float offset_y)
{
    D2D_MATRIX_3X2_F matrix = matrix_identity();

    matrix._31 = offset_x;
    matrix._32 = offset_y;
    return matrix;
}

static D2D_MATRIX_3X2_F matrix_scale(float scale_x, float scale_y)
{
    D2D_MATRIX_3X2_F matrix = matrix_identity();

    matrix._11 = scale_x;
    matrix._22 = scale_y;
    return matrix;
}

static D2D_MATRIX_3X2_F matrix_rotate(float angle)
{
    D2D_MATRIX_3X2_F matrix;
    float radians = angle * (float)M_PI / 180.0f;

    matrix._11 = cosf(radians);
    matrix._12 = sinf(radians);
    matrix._21 = -sinf(radians);
    matrix._22 = cosf(radians);
    matrix._31 = 0.0f;
    matrix._32 = 0.0f;
    return matrix;
}

static D2D_MATRIX_3X2_F matrix_skew(float angle_x, float angle_y)
{
    D2D_MATRIX_3X2_F matrix = matrix_identity();

    matrix._21 = tanf(angle_x * (float)M_PI / 180.0f);
    matrix._12 = tanf(angle_y * (float)M_PI / 180.0f);
    return matrix;
}

/* Translate transform */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE translate_transform_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(transform, struct composition_translate_transform, transform);

    return matrix_translate(translate->offset_x, translate->offset_y);
}

static void STDMETHODCALLTYPE translate_transform_destroy(struct composition_transform *transform)
{
    free(CONTAINING_RECORD(transform, struct composition_translate_transform, transform));
}

static HRESULT STDMETHODCALLTYPE translate_transform_QueryInterface(
        IDCompositionTranslateTransform *iface, REFIID iid, void **out)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(iface, struct composition_translate_transform,
                              IDCompositionTranslateTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionTranslateTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&translate->transform);
        *out = &translate->IDCompositionTranslateTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&translate->transform);
        *out = &translate->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE translate_transform_AddRef(IDCompositionTranslateTransform *iface)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(iface, struct composition_translate_transform,
                              IDCompositionTranslateTransform_iface);

    return InterlockedIncrement(&translate->transform.ref);
}

static ULONG STDMETHODCALLTYPE translate_transform_Release(IDCompositionTranslateTransform *iface)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(iface, struct composition_translate_transform,
                              IDCompositionTranslateTransform_iface);
    ULONG ref = InterlockedDecrement(&translate->transform.ref);

    if (!ref)
        translate->transform.lpVtbl->destroy(&translate->transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE translate_transform_SetOffsetXAnimation(
        IDCompositionTranslateTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE translate_transform_SetOffsetX(IDCompositionTranslateTransform *iface,
        float offset_x)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(iface, struct composition_translate_transform,
                              IDCompositionTranslateTransform_iface);

    TRACE("iface %p, offset_x %f.\n", iface, offset_x);

    translate->offset_x = offset_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE translate_transform_SetOffsetYAnimation(
        IDCompositionTranslateTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE translate_transform_SetOffsetY(IDCompositionTranslateTransform *iface,
        float offset_y)
{
    struct composition_translate_transform *translate =
            CONTAINING_RECORD(iface, struct composition_translate_transform,
                              IDCompositionTranslateTransform_iface);

    TRACE("iface %p, offset_y %f.\n", iface, offset_y);

    translate->offset_y = offset_y;
    return S_OK;
}

static const IDCompositionTranslateTransformVtbl translate_transform_vtbl =
{
    translate_transform_QueryInterface,
    translate_transform_AddRef,
    translate_transform_Release,
    translate_transform_SetOffsetXAnimation,
    translate_transform_SetOffsetX,
    translate_transform_SetOffsetYAnimation,
    translate_transform_SetOffsetY,
};

static const struct composition_transform_vtbl translate_transform_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    translate_transform_compute_matrix,
    translate_transform_destroy,
};

HRESULT composition_transform_create_translate(IDCompositionTranslateTransform **out)
{
    struct composition_translate_transform *translate;

    translate = calloc(1, sizeof(*translate));
    if (!translate)
        return E_OUTOFMEMORY;

    translate->transform.lpVtbl = &translate_transform_private_vtbl;
    translate->transform.ref = 1;
    translate->IDCompositionTranslateTransform_iface.lpVtbl = &translate_transform_vtbl;
    *out = &translate->IDCompositionTranslateTransform_iface;
    return S_OK;
}

/* Scale transform */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE scale_transform_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(transform, struct composition_scale_transform, transform);
    D2D_MATRIX_3X2_F matrix = matrix_scale(scale->scale_x, scale->scale_y);

    return matrix_around_center(&matrix, scale->center_x, scale->center_y);
}

static void STDMETHODCALLTYPE scale_transform_destroy(struct composition_transform *transform)
{
    free(CONTAINING_RECORD(transform, struct composition_scale_transform, transform));
}

static HRESULT STDMETHODCALLTYPE scale_transform_QueryInterface(IDCompositionScaleTransform *iface,
        REFIID iid, void **out)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionScaleTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&scale->transform);
        *out = &scale->IDCompositionScaleTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&scale->transform);
        *out = &scale->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE scale_transform_AddRef(IDCompositionScaleTransform *iface)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    return InterlockedIncrement(&scale->transform.ref);
}

static ULONG STDMETHODCALLTYPE scale_transform_Release(IDCompositionScaleTransform *iface)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);
    ULONG ref = InterlockedDecrement(&scale->transform.ref);

    if (!ref)
        scale->transform.lpVtbl->destroy(&scale->transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetScaleXAnimation(
        IDCompositionScaleTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetScaleX(IDCompositionScaleTransform *iface, float scale_x)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    TRACE("iface %p, scale_x %f.\n", iface, scale_x);

    scale->scale_x = scale_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetScaleYAnimation(
        IDCompositionScaleTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetScaleY(IDCompositionScaleTransform *iface, float scale_y)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    TRACE("iface %p, scale_y %f.\n", iface, scale_y);

    scale->scale_y = scale_y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetCenterXAnimation(
        IDCompositionScaleTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetCenterX(IDCompositionScaleTransform *iface,
        float center_x)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    TRACE("iface %p, center_x %f.\n", iface, center_x);

    scale->center_x = center_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetCenterYAnimation(
        IDCompositionScaleTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE scale_transform_SetCenterY(IDCompositionScaleTransform *iface,
        float center_y)
{
    struct composition_scale_transform *scale =
            CONTAINING_RECORD(iface, struct composition_scale_transform,
                              IDCompositionScaleTransform_iface);

    TRACE("iface %p, center_y %f.\n", iface, center_y);

    scale->center_y = center_y;
    return S_OK;
}

static const IDCompositionScaleTransformVtbl scale_transform_vtbl =
{
    scale_transform_QueryInterface,
    scale_transform_AddRef,
    scale_transform_Release,
    scale_transform_SetScaleXAnimation,
    scale_transform_SetScaleX,
    scale_transform_SetScaleYAnimation,
    scale_transform_SetScaleY,
    scale_transform_SetCenterXAnimation,
    scale_transform_SetCenterX,
    scale_transform_SetCenterYAnimation,
    scale_transform_SetCenterY,
};

static const struct composition_transform_vtbl scale_transform_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    scale_transform_compute_matrix,
    scale_transform_destroy,
};

HRESULT composition_transform_create_scale(IDCompositionScaleTransform **out)
{
    struct composition_scale_transform *scale;

    scale = calloc(1, sizeof(*scale));
    if (!scale)
        return E_OUTOFMEMORY;

    scale->transform.lpVtbl = &scale_transform_private_vtbl;
    scale->transform.ref = 1;
    scale->scale_x = 1.0f;
    scale->scale_y = 1.0f;
    scale->IDCompositionScaleTransform_iface.lpVtbl = &scale_transform_vtbl;
    *out = &scale->IDCompositionScaleTransform_iface;
    return S_OK;
}

/* Rotate transform */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE rotate_transform_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(transform, struct composition_rotate_transform, transform);
    D2D_MATRIX_3X2_F matrix = matrix_rotate(rotate->angle);

    return matrix_around_center(&matrix, rotate->center_x, rotate->center_y);
}

static void STDMETHODCALLTYPE rotate_transform_destroy(struct composition_transform *transform)
{
    free(CONTAINING_RECORD(transform, struct composition_rotate_transform, transform));
}

static HRESULT STDMETHODCALLTYPE rotate_transform_QueryInterface(IDCompositionRotateTransform *iface,
        REFIID iid, void **out)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionRotateTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&rotate->transform);
        *out = &rotate->IDCompositionRotateTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&rotate->transform);
        *out = &rotate->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE rotate_transform_AddRef(IDCompositionRotateTransform *iface)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);

    return InterlockedIncrement(&rotate->transform.ref);
}

static ULONG STDMETHODCALLTYPE rotate_transform_Release(IDCompositionRotateTransform *iface)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);
    ULONG ref = InterlockedDecrement(&rotate->transform.ref);

    if (!ref)
        rotate->transform.lpVtbl->destroy(&rotate->transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetAngleAnimation(
        IDCompositionRotateTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetAngle(IDCompositionRotateTransform *iface,
        float angle)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);

    TRACE("iface %p, angle %f.\n", iface, angle);

    rotate->angle = angle;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetCenterXAnimation(
        IDCompositionRotateTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetCenterX(IDCompositionRotateTransform *iface,
        float center_x)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);

    TRACE("iface %p, center_x %f.\n", iface, center_x);

    rotate->center_x = center_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetCenterYAnimation(
        IDCompositionRotateTransform *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE rotate_transform_SetCenterY(IDCompositionRotateTransform *iface,
        float center_y)
{
    struct composition_rotate_transform *rotate =
            CONTAINING_RECORD(iface, struct composition_rotate_transform,
                              IDCompositionRotateTransform_iface);

    TRACE("iface %p, center_y %f.\n", iface, center_y);

    rotate->center_y = center_y;
    return S_OK;
}

static const IDCompositionRotateTransformVtbl rotate_transform_vtbl =
{
    rotate_transform_QueryInterface,
    rotate_transform_AddRef,
    rotate_transform_Release,
    rotate_transform_SetAngleAnimation,
    rotate_transform_SetAngle,
    rotate_transform_SetCenterXAnimation,
    rotate_transform_SetCenterX,
    rotate_transform_SetCenterYAnimation,
    rotate_transform_SetCenterY,
};

static const struct composition_transform_vtbl rotate_transform_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    rotate_transform_compute_matrix,
    rotate_transform_destroy,
};

HRESULT composition_transform_create_rotate(IDCompositionRotateTransform **out)
{
    struct composition_rotate_transform *rotate;

    rotate = calloc(1, sizeof(*rotate));
    if (!rotate)
        return E_OUTOFMEMORY;

    rotate->transform.lpVtbl = &rotate_transform_private_vtbl;
    rotate->transform.ref = 1;
    rotate->IDCompositionRotateTransform_iface.lpVtbl = &rotate_transform_vtbl;
    *out = &rotate->IDCompositionRotateTransform_iface;
    return S_OK;
}

/* Skew transform */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE skew_transform_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(transform, struct composition_skew_transform, transform);
    D2D_MATRIX_3X2_F matrix = matrix_skew(skew->angle_x, skew->angle_y);

    return matrix_around_center(&matrix, skew->center_x, skew->center_y);
}

static void STDMETHODCALLTYPE skew_transform_destroy(struct composition_transform *transform)
{
    free(CONTAINING_RECORD(transform, struct composition_skew_transform, transform));
}

static HRESULT STDMETHODCALLTYPE skew_transform_QueryInterface(IDCompositionSkewTransform *iface,
        REFIID iid, void **out)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionSkewTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&skew->transform);
        *out = &skew->IDCompositionSkewTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&skew->transform);
        *out = &skew->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE skew_transform_AddRef(IDCompositionSkewTransform *iface)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    return InterlockedIncrement(&skew->transform.ref);
}

static ULONG STDMETHODCALLTYPE skew_transform_Release(IDCompositionSkewTransform *iface)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);
    ULONG ref = InterlockedDecrement(&skew->transform.ref);

    if (!ref)
        skew->transform.lpVtbl->destroy(&skew->transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetAngleXAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetAngleX(IDCompositionSkewTransform *iface,
        float angle_x)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    TRACE("iface %p, angle_x %f.\n", iface, angle_x);

    skew->angle_x = angle_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetAngleYAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetAngleY(IDCompositionSkewTransform *iface,
        float angle_y)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    TRACE("iface %p, angle_y %f.\n", iface, angle_y);

    skew->angle_y = angle_y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetCenterXAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetCenterX(IDCompositionSkewTransform *iface,
        float center_x)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    TRACE("iface %p, center_x %f.\n", iface, center_x);

    skew->center_x = center_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetCenterYAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE skew_transform_SetCenterY(IDCompositionSkewTransform *iface,
        float center_y)
{
    struct composition_skew_transform *skew =
            CONTAINING_RECORD(iface, struct composition_skew_transform,
                              IDCompositionSkewTransform_iface);

    TRACE("iface %p, center_y %f.\n", iface, center_y);

    skew->center_y = center_y;
    return S_OK;
}

static const IDCompositionSkewTransformVtbl skew_transform_vtbl =
{
    skew_transform_QueryInterface,
    skew_transform_AddRef,
    skew_transform_Release,
    skew_transform_SetAngleXAnimation,
    skew_transform_SetAngleX,
    skew_transform_SetAngleYAnimation,
    skew_transform_SetAngleY,
    skew_transform_SetCenterXAnimation,
    skew_transform_SetCenterX,
    skew_transform_SetCenterYAnimation,
    skew_transform_SetCenterY,
};

static const struct composition_transform_vtbl skew_transform_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    skew_transform_compute_matrix,
    skew_transform_destroy,
};

HRESULT composition_transform_create_skew(IDCompositionSkewTransform **out)
{
    struct composition_skew_transform *skew;

    skew = calloc(1, sizeof(*skew));
    if (!skew)
        return E_OUTOFMEMORY;

    skew->transform.lpVtbl = &skew_transform_private_vtbl;
    skew->transform.ref = 1;
    skew->IDCompositionSkewTransform_iface.lpVtbl = &skew_transform_vtbl;
    *out = &skew->IDCompositionSkewTransform_iface;
    return S_OK;
}

/* Matrix transform */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE matrix_transform_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_matrix_transform *matrix =
            CONTAINING_RECORD(transform, struct composition_matrix_transform, transform);

    return matrix->matrix;
}

static void STDMETHODCALLTYPE matrix_transform_destroy(struct composition_transform *transform)
{
    free(CONTAINING_RECORD(transform, struct composition_matrix_transform, transform));
}

static HRESULT STDMETHODCALLTYPE matrix_transform_QueryInterface(IDCompositionMatrixTransform *iface,
        REFIID iid, void **out)
{
    struct composition_matrix_transform *matrix =
            CONTAINING_RECORD(iface, struct composition_matrix_transform,
                              IDCompositionMatrixTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionMatrixTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&matrix->transform);
        *out = &matrix->IDCompositionMatrixTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&matrix->transform);
        *out = &matrix->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE matrix_transform_AddRef(IDCompositionMatrixTransform *iface)
{
    struct composition_matrix_transform *matrix =
            CONTAINING_RECORD(iface, struct composition_matrix_transform,
                              IDCompositionMatrixTransform_iface);

    return InterlockedIncrement(&matrix->transform.ref);
}

static ULONG STDMETHODCALLTYPE matrix_transform_Release(IDCompositionMatrixTransform *iface)
{
    struct composition_matrix_transform *matrix =
            CONTAINING_RECORD(iface, struct composition_matrix_transform,
                              IDCompositionMatrixTransform_iface);
    ULONG ref = InterlockedDecrement(&matrix->transform.ref);

    if (!ref)
        matrix->transform.lpVtbl->destroy(&matrix->transform);
    return ref;
}

static HRESULT STDMETHODCALLTYPE matrix_transform_SetMatrix(IDCompositionMatrixTransform *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct composition_matrix_transform *matrix_transform =
            CONTAINING_RECORD(iface, struct composition_matrix_transform,
                              IDCompositionMatrixTransform_iface);

    TRACE("iface %p, matrix %p.\n", iface, matrix);

    if (!matrix)
        return E_INVALIDARG;

    matrix_transform->matrix = *matrix;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE matrix_transform_SetMatrixElementAnimation(
        IDCompositionMatrixTransform *iface, int row, int column, IDCompositionAnimation *animation)
{
    FIXME("iface %p, row %d, column %d, animation %p stub!\n", iface, row, column, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE matrix_transform_SetMatrixElement(IDCompositionMatrixTransform *iface,
        int row, int column, float value)
{
    struct composition_matrix_transform *matrix =
            CONTAINING_RECORD(iface, struct composition_matrix_transform,
                              IDCompositionMatrixTransform_iface);

    TRACE("iface %p, row %d, column %d, value %f.\n", iface, row, column, value);

    if (row < 0 || row > 2 || column < 0 || column > 1)
        return E_INVALIDARG;

    ((float *)&matrix->matrix)[row * 2 + column] = value;
    return S_OK;
}

static const IDCompositionMatrixTransformVtbl matrix_transform_vtbl =
{
    matrix_transform_QueryInterface,
    matrix_transform_AddRef,
    matrix_transform_Release,
    matrix_transform_SetMatrix,
    matrix_transform_SetMatrixElementAnimation,
    matrix_transform_SetMatrixElement,
};

static const struct composition_transform_vtbl matrix_transform_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    matrix_transform_compute_matrix,
    matrix_transform_destroy,
};

HRESULT composition_transform_create_matrix(IDCompositionMatrixTransform **out)
{
    struct composition_matrix_transform *matrix;

    matrix = calloc(1, sizeof(*matrix));
    if (!matrix)
        return E_OUTOFMEMORY;

    matrix->transform.lpVtbl = &matrix_transform_private_vtbl;
    matrix->transform.ref = 1;
    matrix->matrix = matrix_identity();
    matrix->IDCompositionMatrixTransform_iface.lpVtbl = &matrix_transform_vtbl;
    *out = &matrix->IDCompositionMatrixTransform_iface;
    return S_OK;
}

/* Transform group */

static D2D_MATRIX_3X2_F STDMETHODCALLTYPE transform_group_compute_matrix(
        struct composition_transform *transform)
{
    struct composition_transform_group *group =
            CONTAINING_RECORD(transform, struct composition_transform_group, transform);
    D2D_MATRIX_3X2_F matrix = matrix_identity();
    D2D_MATRIX_3X2_F element;
    UINT i;

    for (i = 0; i < group->elements; ++i)
    {
        composition_transform_get_matrix(group->transforms[i], &element);
        matrix = matrix_multiply(&matrix, &element);
    }

    return matrix;
}

static void STDMETHODCALLTYPE transform_group_destroy(struct composition_transform *transform)
{
    struct composition_transform_group *group =
            CONTAINING_RECORD(transform, struct composition_transform_group, transform);
    UINT i;

    for (i = 0; i < group->elements; ++i)
    {
        if (group->transforms[i])
            composition_transform_release(group->transforms[i]);
    }
    free(group->transforms);
    free(group);
}

static HRESULT STDMETHODCALLTYPE transform_group_QueryInterface(IDCompositionTransform *iface,
        REFIID iid, void **out)
{
    struct composition_transform_group *group =
            CONTAINING_RECORD(iface, struct composition_transform_group,
                              IDCompositionTransform_iface);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IDCompositionTransform)
            || IsEqualGUID(iid, &IID_IDCompositionTransform3D)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        composition_transform_add_ref(&group->transform);
        *out = &group->IDCompositionTransform_iface;
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_ICompositionTransformPrivate))
    {
        composition_transform_add_ref(&group->transform);
        *out = &group->transform;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE transform_group_AddRef(IDCompositionTransform *iface)
{
    struct composition_transform_group *group =
            CONTAINING_RECORD(iface, struct composition_transform_group,
                              IDCompositionTransform_iface);

    return InterlockedIncrement(&group->transform.ref);
}

static ULONG STDMETHODCALLTYPE transform_group_Release(IDCompositionTransform *iface)
{
    struct composition_transform_group *group =
            CONTAINING_RECORD(iface, struct composition_transform_group,
                              IDCompositionTransform_iface);
    ULONG ref = InterlockedDecrement(&group->transform.ref);

    if (!ref)
        group->transform.lpVtbl->destroy(&group->transform);
    return ref;
}

static const IDCompositionTransformVtbl transform_group_vtbl =
{
    transform_group_QueryInterface,
    transform_group_AddRef,
    transform_group_Release,
};

static const struct composition_transform_vtbl transform_group_private_vtbl =
{
    transform_private_QueryInterface,
    transform_private_AddRef,
    transform_private_Release,
    transform_private_GetMatrix,
    transform_group_compute_matrix,
    transform_group_destroy,
};

HRESULT composition_transform_create_group(IDCompositionTransform **transforms, UINT elements,
                                           IDCompositionTransform **out)
{
    struct composition_transform_group *group;
    UINT i;

    if (!transforms || !elements)
        return E_INVALIDARG;

    group = calloc(1, sizeof(*group));
    if (!group)
        return E_OUTOFMEMORY;

    group->transforms = calloc(elements, sizeof(*group->transforms));
    if (!group->transforms)
    {
        free(group);
        return E_OUTOFMEMORY;
    }

    group->transform.lpVtbl = &transform_group_private_vtbl;
    group->transform.ref = 1;
    group->elements = elements;
    group->IDCompositionTransform_iface.lpVtbl = &transform_group_vtbl;

    for (i = 0; i < elements; ++i)
    {
        if (transforms[i])
        {
            group->transforms[i] = composition_transform_from_object(transforms[i]);
            if (!group->transforms[i])
            {
                transform_group_destroy(&group->transform);
                return E_NOTIMPL;
            }
        }
    }

    *out = &group->IDCompositionTransform_iface;
    return S_OK;
}
