/* Direct presentation into an unmodified VcXsrv outer window. */
#ifndef MOCO_COGPU_PRESENT_H
#define MOCO_COGPU_PRESENT_H

struct cogpu_vrend_resource_info;
struct copresent_record;

int cogpu_present_probe(unsigned int duration_ms);

/* R1 is explicitly enabled by the daemon command line. A frame is sampled
 * synchronously while serve() owns the resource; resource_unref() calls the
 * matching release hook before virglrenderer is allowed to delete it. */
void cogpu_present_r1_enable(void);
int  cogpu_present_r1_poll(void);
int  cogpu_present_r1_frame(const struct cogpu_vrend_resource_info *resource,
			    unsigned int level,
			    unsigned int x, unsigned int y,
			    unsigned int width, unsigned int height);
void cogpu_present_r1_resource_unref(unsigned int resource_id);
void cogpu_present_r1_fini(void);

/* R2 consumes metadata records whose acquire fence has already retired in the
 * guest broker. RELEASE is returned only after the host has finished sampling
 * the texture, so buffer reuse remains explicit in both directions. */
void cogpu_present_r2_enable(void);
int  cogpu_present_r2_poll(void);
int  cogpu_present_r2_bind(const struct copresent_record *record,
			   const struct cogpu_vrend_resource_info *resource);
int  cogpu_present_r2_present(const struct copresent_record *record);
int  cogpu_present_r2_unbind(const struct copresent_record *record);
void cogpu_present_r2_reset(unsigned int generation);
void cogpu_present_r2_resource_unref(unsigned int resource_id);
void cogpu_present_r2_fini(void);

#endif
