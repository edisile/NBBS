#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <pthread.h>
#include "nballoc.h"
#include "utils.h"



/*********************************************     
*       MASKS FOR ACCESSING NODE BITMAPS
*********************************************

Bitmap for each node:

  32-----------5-----------4--------------------3--------------------2--------------1--------------0
  |  DONT CARE | OCCUPANCY | PENDING COALESCING | PENDING COALESCING | OCCUPANCY OF | OCCUPANCY OF |
  |            |           | OPS ON LEFT  CHILD | OPS ON RIGHT CHILD | LEFT CHILD   | RIGHT CHILD  |
  |------------|-----------|--------------------|--------------------|--------------|--------------|

*/

#define FREE_BLOCK                  ( 0x0U  )
#define MASK_OCCUPY_RIGHT           ( 0x1U  )
#define MASK_OCCUPY_LEFT            ( 0x2U  )
#define MASK_RIGHT_COALESCE         ( 0x4U  )
#define MASK_LEFT_COALESCE          ( 0x8U  )
#define OCCUPY                      ( 0x10U )

#define MASK_CLEAN_LEFT_COALESCE    (~MASK_LEFT_COALESCE)
#define MASK_CLEAN_RIGHT_COALESCE   (~MASK_RIGHT_COALESCE)
#define OCCUPY_BLOCK                ((OCCUPY) | (MASK_OCCUPY_LEFT) | (MASK_OCCUPY_RIGHT))
#define MASK_CLEAN_OCCUPIED_LEFT    (~MASK_OCCUPY_LEFT )
#define MASK_CLEAN_OCCUPIED_RIGHT   (~MASK_OCCUPY_RIGHT)

#define ROOT            (tree[1])

#define lchild_idx_by_ptr(n)   (((n)->pos)*2)
#define rchild_idx_by_ptr(n)   (lchild_idx_by_ptr(n)+1)
#define parent_idx_by_ptr(n)   (((n)->pos)/2)

#define lchild_idx_by_idx(n)   (n << 1)
#define rchild_idx_by_idx(n)   (lchild_idx_by_idx(n)+1)
#define parent_idx_by_idx(n)   (n >> 1)

#define lchild_ptr_by_ptr(n)   (tree[lchild_idx_by_ptr(n)])
#define rchild_ptr_by_ptr(n)   (tree[rchild_idx_by_ptr(n)])
#define parent_ptr_by_ptr(n)   (tree[parent_idx_by_ptr(n)])

#define lchild_ptr_by_idx(n)   (tree[lchild_idx_by_idx(n)])
#define rchild_ptr_by_idx(n)   (tree[rchild_idx_by_idx(n)])
#define parent_ptr_by_idx(n)   (tree[parent_idx_by_idx(n)])

#define is_left_by_idx(n)      (1ULL & (~(n)))

#define level(n)        ( (overall_height) - (log2_(( (n)->mem_size) / (MIN_ALLOCABLE_BYTES )) ))
#define level_by_idx(n) ( 1 + (log2_(n)))


/***************************************************
*               LOCAL VARIABLES
***************************************************/

static node* volatile tree = NULL; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1]
static node* volatile free_tree = NULL; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1]
static node* volatile real_tree = NULL; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1]
static node* volatile real_free_tree = NULL; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1]
static unsigned long overall_memory_size;
static unsigned int number_of_nodes; //questo non tiene presente che tree[0] è dummy! se qua c'è scritto 7 vuol dire che ci sono 7 nodi UTILIZZABILI
unsigned int number_of_leaves; //questo non tiene presente che tree[0] è dummy! se qua c'è scritto 7 vuol dire che ci sono 7 nodi UTILIZZABILI
static void* volatile overall_memory = NULL;
static volatile unsigned long levels = NUM_LEVELS;
//node* trying;
//node* upper_bound;
//unsigned int failed_at_node;
static unsigned int overall_height;
static unsigned int max_level;

volatile int init_phase = 0;

//extern int number_of_processes;
extern taken_list* takenn;

static void init_tree(unsigned long number_of_nodes);
static unsigned int alloc2(unsigned int, long long size);
//static unsigned int check_parent(node* n);
//static unsigned int alloc(node* n);
//static void smarca(node* n, node* upper_bound);
//static void external_smarca(node* n);
//static void internal_free_node(node* n, node* upper_bound);
static void internal_free_node2(unsigned int n, unsigned int upper_bound);

#ifdef DEBUG
nbint  *size_allocated;
unsigned long long *node_allocated;
#endif

__thread unsigned int tid=-1;
unsigned int partecipants=0;
/*******************************************************************
                INIT NB-BUDDY SYSTEM
*******************************************************************/


/*
 This function build the Non-Blocking Buddy System.

 @Author: Andrea Scarselli
 @param levels: Number of levels in the tree. 
 */
void init(){
    void *tmp_overall_memory;
    void *tmp_real_tree;
    void *tmp_tree;
    void *tmp_real_free_tree;
    void *tmp_free_tree;
    bool first = false;
    number_of_nodes = (1<<levels) -1;
    
    overall_height = levels;
    
    number_of_leaves = 1 << (levels - 1);
    
    overall_memory_size = MIN_ALLOCABLE_BYTES * number_of_leaves;
    
    if(overall_memory_size < MAX_ALLOCABLE_BYTES){
		printf("Not enough levels\n");
		abort();
	}
	
	max_level = overall_height - log2_(MAX_ALLOCABLE_BYTES/MIN_ALLOCABLE_BYTES);
    
   if(init_phase ==  0 && __sync_bool_compare_and_swap(&init_phase, 0, 1)){

        tmp_overall_memory = mmap(NULL, overall_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        first = true;
        if(tmp_overall_memory == MAP_FAILED)
            abort();
        else if(!__sync_bool_compare_and_swap(&overall_memory, NULL, tmp_overall_memory))
                munmap(tmp_overall_memory, overall_memory_size);
           
        tmp_real_tree = mmap(NULL,64+(1+number_of_nodes)*sizeof(node), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        
        if(tmp_real_tree == MAP_FAILED)
            abort();
        else if(!__sync_bool_compare_and_swap(&real_tree, NULL, tmp_real_tree))
                munmap(tmp_real_tree, 64+(1+number_of_nodes)*sizeof(node));

        // DO IN CAS
        tmp_tree = tmp_real_tree;// (typeof(tree)) (( ( (unsigned long long) real_tree) + 64 ) & ( ~ 0x40ULL ) );


        tmp_real_free_tree = mmap(NULL,64+(number_of_leaves)*sizeof(node), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        
        if(tmp_real_free_tree == MAP_FAILED)
            abort();
        else if(!__sync_bool_compare_and_swap(&real_free_tree, NULL, tmp_real_free_tree))
                munmap(tmp_real_free_tree, 64+(number_of_leaves)*sizeof(node));

        // DO IN CAS
        tmp_free_tree = tmp_real_free_tree; //(typeof(tree)) (( ( (unsigned long long) tmp_real_free_tree) + 64 ) & ( ~ 0x40ULL ) );


#ifdef DEBUG
    node_allocated = mmap(NULL, sizeof(unsigned long long), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    size_allocated = mmap(NULL, sizeof(nbint), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      
    __sync_fetch_and_and(node_allocated,0);
    __sync_fetch_and_and(size_allocated,0);
    
    printf("Debug mode: ON\n");
#endif


        __sync_bool_compare_and_swap(&tree, NULL, tmp_tree);
        __sync_bool_compare_and_swap(&free_tree, NULL, tmp_free_tree);
        __sync_bool_compare_and_swap(&init_phase, 1, 2);
    }

    while(init_phase < 2);
    if(init_phase == 2)
        init_tree(number_of_nodes);
    else
        return;
    


    
    if(first){
        printf("dsa-wf: UMA Init complete\n");
        printf("\t Total Memory = %lu\n", overall_memory_size);
        printf("\t Levels = %u\n", overall_height);
        printf("\t Leaves = %u\n", (number_of_nodes+1)/2);
        printf("\t Min size %llu at level %u\n", MIN_ALLOCABLE_BYTES, overall_height);
        printf("\t Max size %llu at level %u\n", MAX_ALLOCABLE_BYTES, overall_height - log2_(MAX_ALLOCABLE_BYTES/MIN_ALLOCABLE_BYTES));
    }
}



void __attribute__ ((constructor(1))) premain(){
    init();
}


/*
 This function inits a static tree represented as an implicit binary heap. 
 The first node at index 0 is a dummy node.

 @Author: Andrea Scarselli
 @param number_of_nodes: the number of valid nodes in the tree.
 */
static void init_tree(unsigned long number_of_nodes){
    printf("NUM NODES %lu\n", number_of_nodes);
    int i=0;

    ROOT.mem_start = 0;
    ROOT.mem_size = overall_memory_size;
    ROOT.pos = 1;
    ROOT.val = (number_of_nodes+1)/2;;
    ROOT.num_leafs = (number_of_nodes+1)/2;

    for(i=2;i<=number_of_nodes;i++){
        tree[i].pos = i;
        node parent = parent_ptr_by_idx(i);
        tree[i].mem_size         = parent.mem_size / 2;
        tree[i].num_leafs        = parent.num_leafs / 2;
        tree[i].val              = parent.num_leafs / 2;
	}
}

//MARK: ALLOCAZIONE

/*
 API for memory allocation.
 
 @Author: Andrea Scarselli
 @param pages: memoria richiesta dall'utente
 @return l'indirizzo di memoria allocato per la richiesta; NULL in caso di fallimento
 
 */
void* bd_xx_malloc(size_t byte){
    unsigned int starting_node, actual;
    unsigned int leaf_position;

    if(tid == -1){
		tid = __sync_fetch_and_add(&partecipants, 1);
     }

    if( byte > MAX_ALLOCABLE_BYTES || byte > overall_memory_size)
        return NULL;
    
    byte = upper_power_of_two(byte);

    if( byte < MIN_ALLOCABLE_BYTES )
        byte = MIN_ALLOCABLE_BYTES;

    starting_node  = overall_memory_size / byte;   
    //printf("STARTNODE %u NUM_LEAF %u LEVEL %u\n", starting_node, tree[starting_node].num_leafs, level_by_idx(starting_node)); 
    actual = alloc2(1, tree[starting_node].num_leafs);
    
    if(actual != 0){
        leaf_position = byte*(actual - overall_memory_size / byte)/MIN_ALLOCABLE_BYTES;
        free_tree[leaf_position].pos = actual;
        //printf("leaf pos %d\n", leaf_position);
        #ifdef DEBUG
            __sync_fetch_and_add(node_allocated,1);
            __sync_fetch_and_add(size_allocated,byte);
        #endif

 //       printf("ALLOC ADDRESS %p INDEX %ld\n", ((char*) overall_memory) + leaf_position*MIN_ALLOCABLE_BYTES, actual);
        return ((char*) overall_memory) + leaf_position*MIN_ALLOCABLE_BYTES; //&tree[actual];
    }        

  //  printf("ALLOC ADDRESS %p INDEX %ld\n", NULL, actual);    
 
    return NULL;
}


/*

 This routine marks the occupancy bit of the respective child for each ancestor of the allocated node.
 This routine returns true if it succeeds to mark every bit from the allocated node to the root, otherwise it returns false.
 Moreover, it resets the coalescing bit.

 Side effect: if it fails the global variable to the invoking thread 'failed_at_node' assumes the value of the first node where the marking has failed.
 @param n: nodo da cui iniziare la risalita (che è già marcato totalmente o parzialmente)
 @return: true if the block has been allocated, otherwise false.
 
 */

/* */

static unsigned int alloc2(unsigned int n, long long s){
   // printf("STARTNODE %u NUM_LEAF %u LEVEL %u VAL %ld REQ %ld\n", n, tree[n].num_leafs, level_by_idx(n), tree[n].val, s); 
    if(tree[n].num_leafs < s) return 0;



    if(__sync_add_and_fetch(&tree[n].val, -s) < 0){
        __sync_add_and_fetch(&tree[n].val, s);
        return 0;
    }

    if(s == tree[n].num_leafs) return n;

    unsigned int tmp = alloc2(n*2, s);
    if(tmp) return tmp;
    tmp = alloc2(n*2+1, s);
    if(tmp) return tmp;
    
     __sync_add_and_fetch(&tree[n].val, s);
    return 0;
}


void bd_xx_free(void* n){
    char * tmp = ((char*)n) - (char*)overall_memory;
    unsigned int pos = (unsigned long long) tmp;
    pos = pos / MIN_ALLOCABLE_BYTES;
    pos = free_tree[pos].pos;
//    printf("FREE ADDRESS %p INDEX %ld TMP %p\n", n, pos, tmp);

    unsigned int child = pos*2;
    long long size = tree[pos].num_leafs;

    while(child<(number_of_nodes+1)){
        tree[child  ].val = tree[child  ].num_leafs;
        tree[child+1].val = tree[child+1].num_leafs;
        child *=2;
    }

    while(pos > 0){
        __sync_fetch_and_add(&tree[pos].val, size);
        pos /= 2;
    }

#ifdef DEBUG
	__sync_fetch_and_add(node_allocated,-1);
	__sync_fetch_and_add(size_allocated,-(n->mem_size));
#endif
}


















//static void smarca(node* n, node* upper_bound){
//    nbint actual_value;
//    nbint new_val;
//    bool is_left_child;
//    node *actual = n, *son = n;//&parent(n);
//    
//    do{
//      actual = &parent_ptr_by_ptr(actual);
//        is_left_child = &lchild_ptr_by_ptr(actual) == son;
//    
//      do{
//          actual_value = actual->val;
//          new_val = actual_value;
//          //libero il rispettivo sottoramo su new val
//          
//            //if( (&left(actual)==son && (actual_value & MASK_LEFT_COALESCE)==0) ||
//          //  (&right(actual)==son && (actual_value & MASK_RIGHT_COALESCE)==0)
//          //  )
//            
//            if( (actual_value & (MASK_RIGHT_COALESCE << is_left_child) ) == 0  )
//
//            { //if n è sinistro AND b1=0 || if n è destro AND b1=0...già riallocato
//              return;
//          }
//          
//          //if (&left(actual)==son)
//          //  new_val = new_val & MASK_CLEAN_LEFT_COALESCE & MASK_CLEAN_OCCUPIED_LEFT;
//          //else
//              new_val = new_val & ((MASK_CLEAN_RIGHT_COALESCE | MASK_CLEAN_OCCUPIED_RIGHT) << is_left_child);
//              
//      } while (new_val != actual_value && !__sync_bool_compare_and_swap(&actual->val,actual_value,new_val));
//
//  }while( (actual!=upper_bound) &&
//          //!(&left(actual)==n && (actual->val & MASK_OCCUPY_RIGHT)!=0) &&
//          //!(&right(actual)==n && (actual->val & MASK_OCCUPY_LEFT)!=0) 
//            !( (actual->val & (MASK_OCCUPY_LEFT >> is_left_child) ) != 0 )  
//
//            );
//          //  || level(actual) <= max_level
//          
//  
//
////    if(actual==upper_bound) //se sono arrivato alla radice ho finito
////        return;
////    if(&left(actual)==n && (actual->val & MASK_OCCUPY_RIGHT)!=0) //if n è sinistro AND (parent(n).actual_value.b4=1) Interrompo! Mio nonno deve vedere il sottoramo occupato perchè mio fratello tiene occupato mio padre!!
////        return;
////    else if(&right(actual)==n && (actual->val & MASK_OCCUPY_LEFT)!=0) // if n è destro AND (parent(n).actual_value.b3=2)
////        return;
////    else
////        smarca_(actual);  
//}




//static void internal_free_node(node* n, node* upper_bound){
//    nbint actual_value;
//
//    if( n->val != OCCUPY_BLOCK ){
//        printf("err: il blocco non è occupato\n");
//        return;
//    }
//    
//    node* actual = &parent_ptr_by_ptr(n);
//    node* runner = n;
//    
//    while(runner!=upper_bound){ //  && level(runner) <=max_level
//      actual_value = actual->val; //CONTROLLARE
//        if(&lchild_ptr_by_ptr(actual)==runner)
//          __sync_fetch_and_or(&actual->val, actual_value | MASK_LEFT_COALESCE);
//      else
//          __sync_fetch_and_or(&actual->val, actual_value | MASK_RIGHT_COALESCE);
//        
////        do{
////            actual_value = actual->val;
////            if(&left(actual)==runner)
////                new_value = actual_value | MASK_LEFT_COALESCE;
////            else
////                new_value = actual_value | MASK_RIGHT_COALESCE;
////        }while(!__sync_bool_compare_and_swap(&actual->val,actual_value, new_value)); // TODO use fetch_&_or
//
//        runner = actual;
//        actual = &parent_ptr_by_ptr(actual);
//    }
//    
//    n->val = 0; // TODO aggiungi barriera --- secondo me "__sync_lock_release" va bene
//    //print_in_ampiezza();
//    if(n!=upper_bound)
//        smarca(n, upper_bound);
//}

//static unsigned int check_parent(node* n){
//    unsigned int failed_at_node;
//    nbint actual_value;
//    nbint new_value;
//    bool is_left_child;
//    node *actual = n, *son = n;//&parent(actual);
//    
//    while(actual != &ROOT){ //  && level(actual) <= max_level --secondo me si può fermare appena vede un fratello a 1
//      son = actual;
//      actual = &parent_ptr_by_ptr(actual);
//        is_left_child = &lchild_ptr_by_ptr(actual) == son;
//
//      do{
//          actual_value = actual->val;
//          
//          //Se l'AND con OCCUPY fallisce vuol dire che qualcuno lo ha occupato
//          if((actual_value & OCCUPY)!=0){
//              failed_at_node = actual->pos;
//              //ripristino dal nodo dove sono partito al nodo dove sono arrivato (da trying ad n)
//              //upper_bound = son;
//              internal_free_node(n, son);
//              return failed_at_node;
//          }
//          
//          new_value = actual_value;
//          
//          //if(is_left_child){ //n è sinistro
//          //  new_value = (new_value & MASK_CLEAN_LEFT_COALESCE) | MASK_OCCUPY_LEFT;
//          //  //new_value = new_value | MASK_OCCUPY_LEFT;
//          //}
//          //else{
//          //  new_value = (new_value & MASK_CLEAN_RIGHT_COALESCE) | MASK_OCCUPY_RIGHT;
//          //  //new_value = new_value | MASK_OCCUPY_RIGHT;
//          //}
//
//            new_value = ( new_value & ( MASK_CLEAN_RIGHT_COALESCE << is_left_child ) ) | ( MASK_OCCUPY_RIGHT << is_left_child );
//          //TODO: se new_val=actual_val non serve fare la CAS
//      }while(new_value != actual_value && //CONTROLLA!!!
//              !__sync_bool_compare_and_swap(&actual->val, actual_value, new_value));
//    }
//    return 0;
//}



//static unsigned int alloc(node* n){
//    nbint actual;
//    //actual è il valore dei bit che sono nel nodo prima che ci lavoro
//    actual = n->val;
//    //trying = n;
//    
//    //il nodo è stato parallelamente occupato. Parzialmente o totalmente
//    if(actual != 0 || !__sync_bool_compare_and_swap(&n->val,0,OCCUPY_BLOCK)){
//        //failed_at_node = n->pos;
//        return n->pos;
//    }
//    
//    //if(n==&ROOT)
//    //    return 0;
//    
//    return check_parent(n);
//
////   //ho allocato tutto l'albero oppure sono riuscito a risalire fino alla radice
////    return (n==&ROOT || check_parent(n) == 0);//||level(n) > max_level
//    
////    if(n==&ROOT || check_parent(n)){
////        return true;
////    }
////    else{
////        return false;
////    }
//}











// //MARK: WRITE SU FILE

// /*
//  SCRIVE SU FILE I NODI PRESI DA UN THREAD - FUNZIONE PRETTAMENTE DI DEBUG
//  */
// void write_taken(){
//     char filename[128];
//     sprintf(filename, "./debug/taken_%d.txt", getpid());
//     FILE *f = fopen(filename, "w");
//     unsigned i;
    
//     if (f == NULL){
//         printf("Error opening file!\n");
//         exit(1);
//     }
    
//     taken_list_elem* runner = takenn->head;
    
//     /* print some text */
//     for(i=0;i<takenn->number;i++){
//         fprintf(f, "%u\n", runner->elem->pos);
//         runner=runner->next;
//     }
    
    
    
//     fclose(f);
    
// }



// /*
//  SCRIVE SU FILE LA SITUAZIONE DELL'ALBERO (IN AMPIEZZA) VISTA DA UN CERTO THREAD
//  */
// void write_on_a_file_in_ampiezza(){
//     char filename[128];
//     sprintf(filename, "./debug/tree.txt");
//     FILE *f = fopen(filename, "w");
//     int i;
    
//     if (f == NULL){
//         printf("Error opening file!\n");
//         exit(1);
//     }
    
//     for(i=1;i<=number_of_nodes;i++){
//         node* n = &tree[i];
//         fprintf(f, "(%p) %u val=%lu has %lu B. mem_start in %lu  level is %u\n", (void*)n, tree[i].pos,  tree[i].val , tree[i].mem_size, tree[i].mem_start,  level(n));
//     }
    
//     fclose(f);
// }


// //MARK: PRINT


// /*traversal tramite left and right*/

// void print_in_profondita(node* n){
//     //printf("%u has\n", n->pos);
//     printf("%u has %lu B. mem_start in %lu left is %u right is %u level=%u\n", n->pos, n->mem_size, n->mem_start, left_index(n), right_index(n), level(n));
//     if(left_index(n)<= number_of_nodes){
//         print_in_profondita(&left(n));
//         print_in_profondita(&
//                             right(n));
//     }
// }

// /*Print in ampiezza*/

// void print_in_ampiezza(){
    
//     int i;
//     for(i=1;i<=number_of_nodes;i++){
//         //printf("%p\n", tree[i]);
//         printf("%u has %lu B. mem_start in %lu val is %lu level=%u\n", tree[i].pos, tree[i].mem_size,tree[i].mem_start, tree[i].
//                val, level(&tree[i]));
//         //printf("%u has %lu B\n", tree[i]->pos, tree[i]->mem_size);
//     }
// }
//
//
//void write_on_a_file_in_ampiezza_start(unsigned int iter){
//     char filename[128];
//     
//   sprintf(filename, "./debug/1nb-%u - tree.txt", iter);
//     //sprintf(filename, "./debug/tree.txt");
//     FILE *f = fopen(filename, "w");
//     int i;
//  
//     if (f == NULL){
//         printf("Error opening file!\n");
//         exit(1);
//     }
//  
//     for(i=1;i<=number_of_nodes;i++){
//         node* n = &tree[i];
//         fprintf(f, "\t%d\t %5u val=%2lu has %8lu B. mem_start in %8lu  level is %2u\n",iter, tree[i].pos,  tree[i].val , tree[i].mem_size, tree[i].mem_start,  level(n));
//     
//     if(level(n)!=overall_height && level(n)!= level(&tree[i+1]))
//       fprintf(f,"\n");
//     }
//     
//     
//  
//     fclose(f);
// }
//