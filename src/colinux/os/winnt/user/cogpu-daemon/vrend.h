/* virglrenderer behind the ring. See vrend.c. */
#ifndef __COGPU_VREND_H__
#define __COGPU_VREND_H__

#include <stdint.h>

struct virgl_renderer_resource_create_args;
struct iovec;

/*
 * The part of a virgl resource that the direct presenter is allowed to know.
 * tex_id is meaningful only inside this process and only while the resource
 * remains alive; target/bind/sample count are retained from CREATE_3D because
 * virgl_renderer_resource_get_info() deliberately does not return them.
 */
struct cogpu_vrend_resource_info {
	uint32_t resource_id;
	uint32_t target;
	uint32_t format;
	uint32_t bind;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;
	uint32_t tex_id;
};

#define COGPU_VREND_MAX_PRESENT_HINTS 8
struct cogpu_vrend_present_hint {
	uint32_t resource_id;
	uint32_t level;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct cogpu_vrend_submit_info {
	unsigned int present_hint_count;
	struct cogpu_vrend_present_hint
		present_hints[COGPU_VREND_MAX_PRESENT_HINTS];
};

/*
 * Called when virglrenderer retires a fence. The daemon turns that into a
 * completion on the guest's used ring; the guest reaps it at a cooperative
 * drain point and wakes whatever slept on it.
 */
typedef void (*cogpu_fence_fn)(void *ctx, uint32_t fence);

int	    cogpu_vrend_init(cogpu_fence_fn fence_cb, void *fence_ctx);
int	    cogpu_vrend_ready(void);
const char *cogpu_vrend_renderer(void);

void	    cogpu_vrend_capset(uint32_t set, uint32_t *max_ver, uint32_t *max_size);
void	    cogpu_vrend_fill_caps(uint32_t set, uint32_t version, void *caps);

int	    cogpu_vrend_ctx_create(uint32_t ctx_id, const char *name,
				   uint32_t namelen, uint32_t context_init);
void	    cogpu_vrend_ctx_destroy(uint32_t ctx_id);
void	    cogpu_vrend_ctx_attach(uint32_t ctx_id, uint32_t res_id);
void	    cogpu_vrend_ctx_detach(uint32_t ctx_id, uint32_t res_id);
int	    cogpu_vrend_transfer(int to_host, uint32_t res_id, uint32_t ctx_id,
				 uint32_t level, uint32_t stride,
				 uint32_t layer_stride,
				 uint32_t x, uint32_t y, uint32_t z,
				 uint32_t w, uint32_t h, uint32_t d,
				 uint64_t offset);

int	    cogpu_vrend_submit(uint32_t ctx_id, const void *cmds, uint32_t bytes,
			       struct cogpu_vrend_submit_info *submit_info);

int	    cogpu_vrend_resource_create(struct virgl_renderer_resource_create_args *args);
void	    cogpu_vrend_resource_unref(uint32_t res_id);
int	    cogpu_vrend_resource_info(uint32_t res_id,
					 struct cogpu_vrend_resource_info *info);
int	    cogpu_vrend_attach_iov(uint32_t res_id, struct iovec *iov, int niov);
void	    cogpu_vrend_detach_iov(uint32_t res_id);

void	    cogpu_vrend_poll(void);
uint64_t    cogpu_vrend_fences(void);

#endif
