#include "common.h"
#include "table.h"
#include "error.h"

struct pt_table *pt_table_new()
{
    struct pt_table *ptable;
    
    
    ptable =(struct pt_table*)malloc(sizeof(struct pt_table));
    
    if(ptable == NULL)
    {
        FATAL("ptable == NULL", __FUNCTION__,__FILE__,__LINE__);
        abort();
    }
    
    bzero(ptable,sizeof(struct pt_table));
    
    ptable->granularity = TABLE_NORMAL_COUNT;
    ptable->size = 0;
    ptable->head = (struct pt_table_node**)malloc(sizeof(struct pt_table_node*) * ptable->granularity);
    
    if(ptable->head == NULL)
    {
        FATAL("ptable->head == NULL", __FUNCTION__,__FILE__,__LINE__);
        abort();
    }
    
    bzero(ptable->head, sizeof(struct pt_table_node**) * ptable->granularity);
    
    return ptable;
}


static void pt_table_free_node(struct pt_table_node** pnode)
{
    struct pt_table_node* node;
    struct pt_table_node* tmp;
    node = *pnode;
    
    
    while(node)
    {
        tmp = node->next;
        free(node);
        node = tmp;
    }
    
    *pnode = NULL;
}

void pt_table_clear(struct pt_table *ptable)
{
    uint32_t i;
    
    for(i = 0; i < ptable->granularity; i++)
    {
        pt_table_free_node(&ptable->head[i]);
        ptable->head[i] = NULL;
    }
    
    ptable->size = 0;
}


void pt_table_free(struct pt_table *ptable)
{
    pt_table_clear(ptable);
    
    free(ptable->head);
    free(ptable);
}


static struct pt_table_node* pt_table_node_new()
{
    struct pt_table_node *node = NULL;
    
    node = (struct pt_table_node*)malloc(sizeof(struct pt_table_node));
    if(node == NULL){
        FATAL("node == NULL", __FUNCTION__,__FILE__,__LINE__);
        abort();
    }
    
    bzero(node,sizeof(struct pt_table_node));
    
    return node;
}

void pt_table_insert(struct pt_table *ptable, uint64_t id, void* ptr)
{
    uint32_t index = id % ptable->granularity;
    struct pt_table_node *current;
    if(ptable->head[index] == NULL)
    {
        ptable->head[index] = pt_table_node_new();
        if(ptable->head[index] == NULL){
            FATAL("pt_table_insert pt_table_node_new failed", __FUNCTION__, __FILE__, __LINE__);
            abort();
        }
        ptable->head[index]->id = id;
        ptable->head[index]->ptr = ptr;
        ptable->size++;
    }
    else
    {
        current = ptable->head[index];
        while(current->next != NULL) current = current->next;
        
        current->next = pt_table_node_new();
        current->next->id = id;
        current->next->ptr = ptr;
        ptable->size++;
    }
}


void pt_table_erase(struct pt_table *ptable, uint64_t id)
{
    uint32_t index = id % ptable->granularity;
    struct pt_table_node *current;
    struct pt_table_node *prev;
    current = ptable->head[index];
    prev = NULL;
    
    
    if(current)
    {
        do
        {
            if(current->id == id)
            {
                if(prev)
                {
                    ptable->size--;
                    prev->next = current->next;
                }
                else
                {
                    ptable->size--;
                    ptable->head[index] = current->next;
                }
                
                free(current);
                break;
            }
            
            prev = current;
            current = current->next;
        }while(current);
    }	
}

void* pt_table_find(struct pt_table *ptable, uint64_t id)
{
    void* ptr = NULL;
    uint32_t index = id % ptable->granularity;
    struct pt_table_node *current;
    
    current = ptable->head[index];
    
    while(current)
    {
        if(current->id == id){
            ptr = current->ptr;
            break;
        }
        
        current = current->next;
    }
    
    return ptr;
}



uint32_t pt_table_size(struct pt_table *ptable)
{
    return ptable->size;
}
